#include "ui_cellular.h"

#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_nrf9151_manager.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NRF9151_UART_DEV K230_NRF9151_UART_DEV
#define NRF9151_UART_BAUD B115200
#define NRF9151_EN_GPIO 2U
#define NRF9151_EN_CONTROL_ENABLED 1
#define K230_IOMUX_BASE 0x91105000UL
#define K230_IOMUX_SIZE 0x1000UL
#define K230_IOMUX_IO2_OFFSET (2U * 4U)
#define K230_IOMUX_IO28_OFFSET (28U * 4U)
#define K230_IOMUX_IO29_OFFSET (29U * 4U)
#define K230_IOMUX_IO50_OFFSET (50U * 4U)
#define K230_IOMUX_IO51_OFFSET (51U * 4U)
#define K230_IOMUX_FUNC_MASK (0x7U << 11)
#define K230_IOMUX_FUNC_ALT0 (0U << 11)
#define K230_IOMUX_FUNC_ALT2 (2U << 11)
#define K230_IOMUX_IE_BIT (1U << 8)
#define K230_IOMUX_OE_BIT (1U << 7)
#define K230_IOMUX_PU_BIT (1U << 6)
#define K230_IOMUX_PD_BIT (1U << 5)
#define K230_IOMUX_ST_BIT (1U << 0)
#define K230_IOMUX_DS_MASK (0xFU << 1)
#define K230_IOMUX_DS_8MA (8U << 1)
#define NRF9151_LOG_MAX 4096
#define NRF9151_LOG_VIEW_MAX 4096
#define NRF9151_TEST_LOG "/tmp/k230_nrf9151_test.log"
#define NRF9151_CMD_TIMEOUT_US 1800000ULL
#define NRF9151_NMEA_READ_SECONDS 15U
#define NRF9151_FULL_TEST_NMEA_SECONDS 20U
#define NRF9151_LTE_PING_READ_SECONDS 12U
#define NRF9151_NMEA_LINE_MAX 192
#define NRF9151_UART_LINE_MAX 512
#define NRF9151_MAX_SATS 48
#define NRF9151_SAT_TEXT_MAX 2048
#define NRF9151_CN0_BAR_MAX 32
#define NRF9151_CN0_MAX 60

typedef enum {
    CELLULAR_ACTION_LINK = 0,
    CELLULAR_ACTION_SIM,
    CELLULAR_ACTION_LTE_STATUS,
    CELLULAR_ACTION_LTE_GNSS_MODE,
    CELLULAR_ACTION_GNSS_START,
    CELLULAR_ACTION_GNSS_STATUS,
    CELLULAR_ACTION_GNSS_NMEA,
    CELLULAR_ACTION_GNSS_STOP,
    CELLULAR_ACTION_FULL_TEST,
    CELLULAR_ACTION_HTTP_GET,
    CELLULAR_ACTION_HTTP_POST,
    CELLULAR_ACTION_MQTT_TEST,
} cellular_action_t;

typedef struct {
    cellular_action_t action;
    unsigned int generation;
} cellular_worker_ctx_t;

typedef struct {
    char talker[3];
    int prn;
    int elevation;
    int azimuth;
    int cn0;
    uint64_t last_seen_us;
    int valid;
} cellular_satellite_t;

typedef struct {
    lv_obj_t *column;
    lv_obj_t *value;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *label;
} cellular_cn0_bar_t;

static const char *const cellular_link_cmds[] = {
    "AT",
    "AT+CMEE=1",
    "ATI",
    "AT+CGSN",
    "AT+CGMR",
};

static const char *const cellular_sim_cmds[] = {
    "AT",
    "AT+CMEE=1",
    "AT+CFUN=1",
    "AT%XSIM=1",
    "AT%XSIM?",
    "AT+CPIN?",
    "AT%XICCID",
    "AT+CIMI",
    "AT+CRSM=176,12258,0,0,10",
};

static const char *const cellular_lte_status_cmds[] = {
    "AT",
    "AT+CMEE=1",
    "AT%XSIM=1",
    "AT%XSIM?",
    "AT+CPIN?",
    "AT+CFUN?",
    "AT%XSYSTEMMODE?",
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

static const char *const cellular_lte_gnss_mode_cmds[] = {
    "AT",
    "AT+CMEE=1",
    "AT%XSIM=1",
    "AT+CFUN=1",
    "AT+CEREG=5",
    "AT%XSIM?",
    "AT+CPIN?",
    "AT%XSYSTEMMODE?",
    "AT+CEREG?",
    "AT+CESQ",
    "AT%XMONITOR",
    "AT+CGATT?",
    "AT+CGACT?",
    "AT+CGPADDR",
};

static const char *const cellular_gnss_start_cmds[] = {
    "AT+CFUN=31",
    "AT%XSYSTEMMODE?",
    "AT#XNMEA=1",
    "AT#XGNSS=1,0,0,0",
};

static const char *const cellular_gnss_status_cmds[] = {
    "AT#XGNSS?",
    "AT#XNMEA?",
    "AT%XSYSTEMMODE?",
};

static const char *const cellular_gnss_stop_cmds[] = {
    "AT#XGNSS=0",
    "AT#XNMEA=0",
};

static lv_obj_t *cellular_status_label;
static lv_obj_t *cellular_link_label;
static lv_obj_t *cellular_sim_label;
static lv_obj_t *cellular_lte_label;
static lv_obj_t *cellular_gnss_label;
static lv_obj_t *cellular_imei_label;
static lv_obj_t *cellular_network_label;
static lv_obj_t *cellular_gps_label;
static lv_obj_t *cellular_ttff_label;
static lv_obj_t *cellular_sat_chart;
static lv_obj_t *cellular_sat_label;
static lv_obj_t *cellular_log_label;
static lv_obj_t *cellular_cno_button;
static lv_obj_t *cellular_check_panel;
static lv_obj_t *cellular_check_dialog;
static lv_obj_t *cellular_check_spinner;
static lv_obj_t *cellular_check_label;
static cellular_cn0_bar_t cellular_cn0_bars[NRF9151_CN0_BAR_MAX];
static lv_timer_t *cellular_timer;

static pthread_mutex_t cellular_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cellular_uart_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static int cellular_worker_active;
static int cellular_cno_monitor_active;
static int cellular_cno_monitor_stop;
static int cellular_manager_gnss_started;
static int cellular_page_active;
static int cellular_check_was_active;
static unsigned int cellular_action_generation;
static char cellular_running_title[64] = "LTE";
static uint64_t cellular_check_hide_us;
static char cellular_status[160] = "Ready";
static char cellular_link_status[160] = "Not tested";
static char cellular_sim_status[160] = "Not tested";
static char cellular_lte_status[160] = "Not tested";
static char cellular_gnss_status[160] = "Not tested";
static char cellular_imei_status[96] = "--";
static char cellular_operator_status[96] = "--";
static char cellular_ip_status[96] = "--";
static char cellular_network_status[192] = "-- / --";
static char cellular_gps_status[256] = "Waiting for NMEA";
static char cellular_sat_status[NRF9151_SAT_TEXT_MAX] = "No satellite data";
static char cellular_log[NRF9151_LOG_MAX];
static unsigned int cellular_urc_count;
static unsigned int cellular_nmea_count;
static int cellular_sim_positive_seen;
static int cellular_lte_sim_fault_seen;
static int cellular_led_auto_pending;
static uint64_t cellular_last_uart_us;
static char cellular_last_nmea[NRF9151_NMEA_LINE_MAX];
static char cellular_last_urc[192];
static char cellular_http_url[256] = "http://example.com/";
static char cellular_http_post_spec[512] = "http://example.com/|hello=k230";
static char cellular_mqtt_spec[512] =
    "broker.hivemq.com|1883|||k230/test|hello from k230|-1";
static cellular_satellite_t cellular_sats[NRF9151_MAX_SATS];
static uint64_t cellular_gnss_start_us;
static uint64_t cellular_gnss_fix_us;
static int cellular_gnss_fix_valid;
static pthread_mutex_t cellular_gpio_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gpiod_chip *cellular_en_gpio_chip;
static struct gpiod_line_request *cellular_en_gpio_request;
static unsigned int cellular_en_gpio_offset;
static int cellular_uart_lock_fd = -1;

static int cellular_response_gnss_active(const char *resp);
static const char *cellular_skip_spaces(const char *s);
static void cellular_lte_update_from_line(const char *line, int rc);
static void cellular_process_response_lines(const char *resp);
static int cellular_try_led_mode(int fd, int mode, const char *reason);
static void cellular_apply_led_auto_if_pending(int fd);

static void cellular_iomux_write(volatile uint32_t *base, unsigned int offset,
                                 uint32_t value, const char *name)
{
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)base + offset);
    uint32_t before = *reg;

    if(before != value) {
        *reg = value;
    }
    fprintf(stderr, "[nrf9151] %s iomux 0x%08x -> 0x%08x\n", name, before,
            value);
}

static int cellular_configure_uart3_iomux(void)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        fprintf(stderr, "[nrf9151] open /dev/mem failed: %s\n",
                strerror(errno));
        return -1;
    }

    map = mmap(NULL, K230_IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               K230_IOMUX_BASE);
    if(map == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        fprintf(stderr, "[nrf9151] mmap iomux failed: %s\n", strerror(errno));
        return -1;
    }

    regs = (volatile uint32_t *)map;
    cellular_iomux_write(regs, K230_IOMUX_IO28_OFFSET,
                         K230_IOMUX_FUNC_ALT2 | K230_IOMUX_IE_BIT |
                             K230_IOMUX_OE_BIT | K230_IOMUX_ST_BIT |
                             K230_IOMUX_DS_8MA,
                         "IO28 UART3_TXD");
    cellular_iomux_write(regs, K230_IOMUX_IO29_OFFSET,
                         K230_IOMUX_FUNC_ALT2 | K230_IOMUX_IE_BIT |
                             K230_IOMUX_ST_BIT | K230_IOMUX_DS_8MA,
                         "IO29 UART3_RXD");
    /* IO50/IO51 are another UART3 mux group on K230. Leaving them on UART3
     * conflicts with the IO28/IO29 UART path used by the keyboard/LTE base. */
    cellular_iomux_write(regs, K230_IOMUX_IO50_OFFSET,
                         K230_IOMUX_FUNC_ALT0 | K230_IOMUX_DS_8MA,
                         "IO50 GPIO");
    cellular_iomux_write(regs, K230_IOMUX_IO51_OFFSET,
                         K230_IOMUX_FUNC_ALT0 | K230_IOMUX_DS_8MA,
                         "IO51 GPIO");

    munmap(map, K230_IOMUX_SIZE);
    close(fd);
    return 0;
}

static int cellular_configure_en_iomux(void)
{
    int fd;
    void *map;
    volatile uint32_t *reg;
    uint32_t before;
    uint32_t after;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        fprintf(stderr, "[nrf9151] open /dev/mem failed: %s\n",
                strerror(errno));
        return -1;
    }

    map = mmap(NULL, K230_IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               K230_IOMUX_BASE);
    if(map == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        fprintf(stderr, "[nrf9151] mmap iomux failed: %s\n", strerror(errno));
        return -1;
    }

    reg = (volatile uint32_t *)((uint8_t *)map + K230_IOMUX_IO2_OFFSET);
    before = *reg;
    after = before;
    after &= ~K230_IOMUX_FUNC_MASK;
    after &= ~(K230_IOMUX_IE_BIT | K230_IOMUX_PU_BIT | K230_IOMUX_PD_BIT);
    after &= ~K230_IOMUX_DS_MASK;
    after |= K230_IOMUX_OE_BIT | K230_IOMUX_DS_8MA;
    if(after != before) {
        *reg = after;
    }
    munmap(map, K230_IOMUX_SIZE);
    close(fd);
    fprintf(stderr, "[nrf9151] IO2 iomux 0x%08x -> 0x%08x\n", before, after);
    return 0;
}

static int cellular_gpio_request_locked(unsigned int gpio, int value)
{
    struct gpiod_chip *chip = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    struct gpiod_line_request *request = NULL;
    unsigned int chip_index = gpio / 32U;
    unsigned int offset = gpio % 32U;
    char chip_path[32];
    int ok = -1;
    int saved_errno = 0;

    if(cellular_en_gpio_request) {
        return 0;
    }

    if(chip_index > 1U) {
        errno = EINVAL;
        return -1;
    }
    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u", chip_index);
    chip = gpiod_chip_open(chip_path);
    if(!chip) {
        return -1;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if(!settings || !line_config || !request_config) {
        saved_errno = errno;
        goto out;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_AS_IS);
    gpiod_line_settings_set_output_value(settings, value ?
                                         GPIOD_LINE_VALUE_ACTIVE :
                                         GPIOD_LINE_VALUE_INACTIVE);
    gpiod_request_config_set_consumer(request_config, "k230-phone-nrf9151-en");
    if(gpiod_line_config_add_line_settings(line_config, &offset, 1,
                                           settings) != 0) {
        saved_errno = errno;
        goto out;
    }

    request = gpiod_chip_request_lines(chip, request_config, line_config);
    if(!request) {
        saved_errno = errno;
        goto out;
    }
    cellular_en_gpio_chip = chip;
    cellular_en_gpio_request = request;
    cellular_en_gpio_offset = offset;
    chip = NULL;
    request = NULL;
    ok = 0;

out:
    if(request) {
        gpiod_line_request_release(request);
    }
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_config);
    gpiod_request_config_free(request_config);
    if(chip) {
        gpiod_chip_close(chip);
    }
    if(saved_errno) {
        errno = saved_errno;
    }
    return ok;
}

static int cellular_gpio_set(unsigned int gpio, int value)
{
    int ok;
    int saved_errno = 0;

    if(gpio == NRF9151_EN_GPIO) {
        cellular_configure_en_iomux();
    }

    pthread_mutex_lock(&cellular_gpio_lock);
    ok = cellular_gpio_request_locked(gpio, value);
    if(ok == 0) {
        ok = gpiod_line_request_set_value(cellular_en_gpio_request,
                                          cellular_en_gpio_offset,
                                          value ? GPIOD_LINE_VALUE_ACTIVE :
                                          GPIOD_LINE_VALUE_INACTIVE);
        if(ok != 0) {
            saved_errno = errno;
        }
    } else {
        saved_errno = errno;
    }
    pthread_mutex_unlock(&cellular_gpio_lock);

    usleep(1000);
    if(saved_errno) {
        errno = saved_errno;
    }
    return ok;
}

static void cellular_file_log(const char *line)
{
    FILE *fp = fopen(NRF9151_TEST_LOG, "a");

    if(!fp) {
        return;
    }
    fprintf(fp, "%llu %s\n", (unsigned long long)ui_monotonic_us(), line);
    fclose(fp);
}

static void cellular_log_append(const char *fmt, ...)
{
    char line[512];
    size_t used;
    size_t add;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&cellular_lock);
    used = strlen(cellular_log);
    add = strlen(line);
    if(used + add + 2U >= sizeof(cellular_log)) {
        size_t keep = sizeof(cellular_log) / 2U;

        memmove(cellular_log, cellular_log + used - keep, keep + 1U);
        used = strlen(cellular_log);
    }
    snprintf(cellular_log + used, sizeof(cellular_log) - used, "%s\n", line);
    pthread_mutex_unlock(&cellular_lock);

    cellular_file_log(line);
}

static void cellular_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&cellular_lock);
    va_start(ap, fmt);
    vsnprintf(cellular_status, sizeof(cellular_status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_set_summary(char *dst, size_t dst_len, const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&cellular_lock);
    va_start(ap, fmt);
    vsnprintf(dst, dst_len, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_network_rebuild_locked(void)
{
    snprintf(cellular_network_status, sizeof(cellular_network_status),
             "%s / %s", cellular_operator_status[0] ?
             cellular_operator_status : "--",
             cellular_ip_status[0] ? cellular_ip_status : "--");
}

static void cellular_set_imei(const char *imei)
{
    pthread_mutex_lock(&cellular_lock);
    snprintf(cellular_imei_status, sizeof(cellular_imei_status), "%s",
             imei && imei[0] ? imei : "--");
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_set_operator(const char *name)
{
    pthread_mutex_lock(&cellular_lock);
    snprintf(cellular_operator_status, sizeof(cellular_operator_status), "%s",
             name && name[0] ? name : "--");
    cellular_network_rebuild_locked();
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_set_ip(const char *ip)
{
    pthread_mutex_lock(&cellular_lock);
    snprintf(cellular_ip_status, sizeof(cellular_ip_status), "%s",
             ip && ip[0] ? ip : "--");
    cellular_network_rebuild_locked();
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_apply_manager_status(const k230_nrf9151_status_t *status)
{
    uint64_t now_us;

    if(!status) {
        return;
    }
    now_us = ui_monotonic_us();

    pthread_mutex_lock(&cellular_lock);
    cellular_cno_monitor_active = k230_nrf9151_gnss_monitor_active();
    snprintf(cellular_link_status, sizeof(cellular_link_status), "%s",
             status->link_ok ?
             (status->firmware[0] && strcmp(status->firmware, "--") != 0 ?
              status->firmware : "OK") :
             (status->present ? "No AT response" : "Not detected"));
    snprintf(cellular_sim_status, sizeof(cellular_sim_status), "%s",
             status->sim_ready ? "OK" :
             (status->sim_status[0] && strcmp(status->sim_status, "--") != 0 ?
              status->sim_status : "Not tested"));
    snprintf(cellular_lte_status, sizeof(cellular_lte_status), "%s",
             status->lte_status[0] ? status->lte_status : "Not tested");
    snprintf(cellular_gnss_status, sizeof(cellular_gnss_status), "%s",
             status->gnss_status[0] ? status->gnss_status :
             (status->gnss_running ? "GNSS running" : "Off"));
    snprintf(cellular_imei_status, sizeof(cellular_imei_status), "%s",
             status->imei[0] ? status->imei : "--");
    snprintf(cellular_operator_status, sizeof(cellular_operator_status), "%s",
             status->operator_name[0] ? status->operator_name : "--");
    snprintf(cellular_ip_status, sizeof(cellular_ip_status), "%s",
             status->ip[0] ? status->ip : "--");
    cellular_network_rebuild_locked();

    cellular_nmea_count = status->nmea_rx_count;
    cellular_urc_count = status->nmea_valid_count;
    if(status->gnss_has_fix) {
        snprintf(cellular_gps_status, sizeof(cellular_gps_status),
                 "Lat %.6f  Lon %.6f  Sats %u",
                 status->latitude, status->longitude, status->satellites);
        cellular_gnss_fix_valid = 1;
        cellular_gnss_fix_us = now_us;
        if(status->ttff_ms > 0UL) {
            cellular_gnss_start_us =
                now_us > status->ttff_ms * 1000ULL ?
                now_us - status->ttff_ms * 1000ULL : now_us;
        } else if(cellular_gnss_start_us == 0ULL) {
            cellular_gnss_start_us = now_us;
        }
    } else {
        snprintf(cellular_gps_status, sizeof(cellular_gps_status),
                 "%s  NMEA rx=%u valid=%u nofix=%u sats=%u",
                 status->gnss_phase[0] ? status->gnss_phase :
                 (status->gnss_running ? "searching" : "off"),
                 status->nmea_rx_count, status->nmea_valid_count,
                 status->nmea_nofix_count, status->satellites);
        if(status->gnss_running && cellular_gnss_start_us == 0ULL) {
            cellular_gnss_start_us = now_us;
        }
        cellular_gnss_fix_valid = 0;
    }
    snprintf(cellular_sat_status, sizeof(cellular_sat_status),
             "NMEA rx=%u valid=%u nofix=%u sats=%u last=%lums",
             status->nmea_rx_count, status->nmea_valid_count,
             status->nmea_nofix_count, status->satellites,
             status->last_nmea_ms);
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_sync_manager_status(void)
{
    k230_nrf9151_status_t status;

    if(k230_nrf9151_read_status(&status, 15) == 0) {
        cellular_apply_manager_status(&status);
    }
}

void ui_cellular_startup(void)
{
#if NRF9151_EN_CONTROL_ENABLED
    if(cellular_gpio_set(NRF9151_EN_GPIO, 1) == 0) {
        cellular_log_append("GPIO%u default EN=1", NRF9151_EN_GPIO);
        cellular_set_status("Power on");
    } else {
        cellular_log_append("GPIO%u default EN failed: %s", NRF9151_EN_GPIO,
                            strerror(errno));
    }
#else
    cellular_log_append("GPIO%u EN control disabled; leave modem power unchanged",
                        NRF9151_EN_GPIO);
    cellular_set_status("EN external/no control");
#endif
}

static void cellular_gnss_reset_locked(void)
{
    memset(cellular_sats, 0, sizeof(cellular_sats));
    cellular_urc_count = 0;
    cellular_nmea_count = 0;
    cellular_last_uart_us = 0;
    cellular_last_nmea[0] = '\0';
    cellular_last_urc[0] = '\0';
    cellular_gnss_start_us = 0;
    cellular_gnss_fix_us = 0;
    cellular_gnss_fix_valid = 0;
    cellular_led_auto_pending = 0;
    snprintf(cellular_gps_status, sizeof(cellular_gps_status),
             "%s", "Waiting for NMEA");
    snprintf(cellular_sat_status, sizeof(cellular_sat_status),
             "%s", "No satellite data");
}

static void cellular_gnss_session_start_locked(void)
{
    memset(cellular_sats, 0, sizeof(cellular_sats));
    cellular_urc_count = 0;
    cellular_nmea_count = 0;
    cellular_last_uart_us = 0;
    cellular_last_nmea[0] = '\0';
    cellular_last_urc[0] = '\0';
    cellular_gnss_start_us = ui_monotonic_us();
    cellular_gnss_fix_us = 0;
    cellular_gnss_fix_valid = 0;
    cellular_led_auto_pending = 0;
    snprintf(cellular_gps_status, sizeof(cellular_gps_status),
             "%s", "Searching for fix");
    snprintf(cellular_sat_status, sizeof(cellular_sat_status),
             "%s", "No satellite data");
}

static int cellular_gnss_note_fix_locked(double *ttff_s)
{
    uint64_t now = ui_monotonic_us();

    if(!cellular_gnss_start_us) {
        cellular_gnss_start_us = now;
        cellular_gnss_fix_us = now;
        cellular_gnss_fix_valid = 1;
        cellular_led_auto_pending = 1;
        if(ttff_s) {
            *ttff_s = 0.0;
        }
        return 1;
    }
    if(cellular_gnss_fix_valid) {
        if(ttff_s) {
            *ttff_s =
                (double)(cellular_gnss_fix_us - cellular_gnss_start_us) /
                1000000.0;
        }
        return 0;
    }
    cellular_gnss_fix_us = now;
    cellular_gnss_fix_valid = 1;
    cellular_led_auto_pending = 1;
    if(ttff_s) {
        *ttff_s = (double)(cellular_gnss_fix_us - cellular_gnss_start_us) /
                  1000000.0;
    }
    return 1;
}

static void cellular_gnss_write_fix_cache(double lat, double lon, int has_alt,
                                          double alt_m, int sats,
                                          const char *source)
{
    if(k230_nrf9151_write_gnss_fix(lat, lon, has_alt, alt_m, sats,
                                   source && source[0] ? source : "lte") != 0) {
        cellular_log_append("GNSS fix cache write failed: %s", strerror(errno));
    }
}

static void cellular_ttff_text(char *out, size_t out_len, uint64_t start_us,
                               uint64_t fix_us, int fix_valid)
{
    uint64_t now;
    double seconds;

    if(!out || out_len == 0) {
        return;
    }
    if(!start_us) {
        snprintf(out, out_len, "TTFF: not started");
        return;
    }
    if(fix_valid && fix_us >= start_us) {
        seconds = (double)(fix_us - start_us) / 1000000.0;
        snprintf(out, out_len, "TTFF: %.1fs", seconds);
        return;
    }
    now = ui_monotonic_us();
    seconds = now > start_us ? (double)(now - start_us) / 1000000.0 : 0.0;
    snprintf(out, out_len, "TTFF: searching %.1fs", seconds);
}

static uint32_t cellular_sat_color(const char *talker)
{
    if(!talker) {
        return 0x9AA4AF;
    }
    if(strcmp(talker, "GP") == 0 || strcmp(talker, "GN") == 0) {
        return 0x25C281;
    }
    if(strcmp(talker, "GL") == 0) {
        return 0x60A5FA;
    }
    if(strcmp(talker, "GB") == 0 || strcmp(talker, "BD") == 0) {
        return 0xF5A524;
    }
    if(strcmp(talker, "GA") == 0) {
        return 0x8B5CF6;
    }
    if(strcmp(talker, "GQ") == 0) {
        return 0xEF4D5A;
    }
    return 0x9AA4AF;
}

static void cellular_cn0_chart_refresh(const cellular_satellite_t *sats,
                                       size_t sat_count)
{
    size_t bar_index = 0;

    if(!cellular_sat_chart || !sats) {
        return;
    }
    for(size_t i = 0; i < sat_count && bar_index < NRF9151_CN0_BAR_MAX; i++) {
        const cellular_satellite_t *sat = &sats[i];
        cellular_cn0_bar_t *bar;
        lv_coord_t track_h;
        lv_coord_t fill_h;
        int cn0;

        if(!sat->valid) {
            continue;
        }
        bar = &cellular_cn0_bars[bar_index];
        cn0 = sat->cn0;
        if(cn0 < 0) {
            cn0 = 0;
        }
        if(cn0 > NRF9151_CN0_MAX) {
            cn0 = NRF9151_CN0_MAX;
        }
        track_h = lv_obj_get_height(bar->track);
        if(track_h <= 0) {
            track_h = 78;
        }
        fill_h = (track_h * cn0) / NRF9151_CN0_MAX;
        if(cn0 > 0 && fill_h < 2) {
            fill_h = 2;
        }
        lv_label_set_text_fmt(bar->value, sat->cn0 > 0 ? "%d" : "--",
                              sat->cn0);
        lv_label_set_text_fmt(bar->label, "%02d", sat->prn);
        lv_obj_set_style_text_color(bar->label,
                                    lv_color_hex(cellular_sat_color(sat->talker)),
                                    0);
        lv_obj_set_style_bg_color(bar->fill,
                                  lv_color_hex(cellular_sat_color(sat->talker)),
                                  0);
        lv_obj_set_height(bar->fill, fill_h);
        lv_obj_align(bar->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(bar->column, LV_OBJ_FLAG_HIDDEN);
        bar_index++;
    }
    for(size_t i = bar_index; i < NRF9151_CN0_BAR_MAX; i++) {
        lv_obj_add_flag(cellular_cn0_bars[i].column, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cellular_cn0_bar_create(lv_obj_t *parent, cellular_cn0_bar_t *bar)
{
    const lv_coord_t col_w = 32;
    const lv_coord_t track_w = 12;

    bar->column = lv_obj_create(parent);
    lv_obj_set_size(bar->column, col_w, 122);
    lv_obj_set_style_bg_opa(bar->column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar->column, 0, 0);
    lv_obj_set_style_radius(bar->column, 0, 0);
    lv_obj_set_style_pad_all(bar->column, 0, 0);
    lv_obj_clear_flag(bar->column, LV_OBJ_FLAG_SCROLLABLE);

    bar->value = ui_label(bar->column, "--", &lv_font_montserrat_12, 0xDCE5EE);
    lv_obj_set_width(bar->value, col_w);
    lv_obj_set_style_text_align(bar->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bar->value, LV_ALIGN_TOP_MID, 0, 0);

    bar->track = lv_obj_create(bar->column);
    lv_obj_set_size(bar->track, track_w, 78);
    lv_obj_set_style_bg_color(bar->track, lv_color_hex(0x26313D), 0);
    lv_obj_set_style_bg_opa(bar->track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar->track, 0, 0);
    lv_obj_set_style_radius(bar->track, 0, 0);
    lv_obj_set_style_pad_all(bar->track, 0, 0);
    lv_obj_clear_flag(bar->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar->track, LV_ALIGN_TOP_MID, 0, 18);

    bar->fill = lv_obj_create(bar->track);
    lv_obj_set_width(bar->fill, LV_PCT(100));
    lv_obj_set_height(bar->fill, 0);
    lv_obj_set_style_bg_color(bar->fill, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_bg_opa(bar->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar->fill, 0, 0);
    lv_obj_set_style_radius(bar->fill, 0, 0);
    lv_obj_set_style_pad_all(bar->fill, 0, 0);
    lv_obj_clear_flag(bar->fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar->fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    bar->label = ui_label(bar->column, "--", &lv_font_montserrat_12, 0x9AA4AF);
    lv_obj_set_width(bar->label, col_w);
    lv_obj_set_style_text_align(bar->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bar->label, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_flag(bar->column, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *cellular_cn0_chart_create(lv_obj_t *parent, int width)
{
    lv_obj_t *chart = lv_obj_create(parent);

    lv_obj_set_size(chart, width, 140);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_pad_top(chart, 4, 0);
    lv_obj_set_style_pad_bottom(chart, 4, 0);
    lv_obj_set_style_pad_left(chart, 6, 0);
    lv_obj_set_style_pad_right(chart, 6, 0);
    lv_obj_set_style_pad_column(chart, 8, 0);
    lv_obj_set_flex_flow(chart, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chart, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(chart, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chart, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    for(size_t i = 0; i < NRF9151_CN0_BAR_MAX; i++) {
        cellular_cn0_bar_create(chart, &cellular_cn0_bars[i]);
    }
    return chart;
}

static void cellular_log_refresh(void)
{
    if(cellular_log_label) {
        char text[NRF9151_LOG_VIEW_MAX];

        pthread_mutex_lock(&cellular_lock);
        snprintf(text, sizeof(text), "%s", cellular_log[0] ?
                 cellular_log : ui_tr("No log yet"));
        pthread_mutex_unlock(&cellular_lock);
        text[sizeof(text) - 1U] = '\0';
        lv_label_set_text(cellular_log_label, text);
    }
}

static void cellular_check_progress_refresh(int active, const char *status,
                                            const char *active_title)
{
    uint64_t now;

    if(!cellular_check_panel || !cellular_check_label ||
       !cellular_check_spinner) {
        return;
    }

    now = ui_monotonic_us();
    if(active) {
        cellular_check_was_active = 1;
        cellular_check_hide_us = 0;
        lv_obj_clear_flag(cellular_check_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cellular_check_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cellular_check_panel);
        lv_label_set_text(cellular_check_label,
                          ui_tr(active_title && active_title[0] ?
                                active_title : "Working"));
        return;
    }

    if(cellular_check_was_active) {
        const char *done = status && strstr(status, "issues") ?
                           "Check issues" : "Check complete";

        cellular_check_was_active = 0;
        cellular_check_hide_us = now + 1200000ULL;
        lv_obj_clear_flag(cellular_check_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cellular_check_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cellular_check_panel);
        lv_label_set_text(cellular_check_label, ui_tr(done));
        return;
    }

    if(cellular_check_hide_us != 0 && now >= cellular_check_hide_us) {
        cellular_check_hide_us = 0;
        lv_obj_add_flag(cellular_check_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cellular_status_refresh(void)
{
    char status[160];
    char link[160];
    char sim[160];
    char lte[160];
    char gnss[160];
    char imei[96];
    char network[192];
    char gps[256];
    char ttff[64];
    char sats[NRF9151_SAT_TEXT_MAX];
    char running_title[64];
    cellular_satellite_t sats_copy[NRF9151_MAX_SATS];
    uint64_t gnss_start_us;
    uint64_t gnss_fix_us;
    int gnss_fix_valid;
    int active;
    int monitor_active;
    int exists = k230_nrf9151_uart_present();

    cellular_sync_manager_status();

    pthread_mutex_lock(&cellular_lock);
    snprintf(status, sizeof(status), "%s", cellular_status);
    snprintf(link, sizeof(link), "%s", cellular_link_status);
    snprintf(sim, sizeof(sim), "%s", cellular_sim_status);
    snprintf(lte, sizeof(lte), "%s", cellular_lte_status);
    snprintf(gnss, sizeof(gnss), "%s", cellular_gnss_status);
    snprintf(imei, sizeof(imei), "%s", cellular_imei_status);
    snprintf(network, sizeof(network), "%s", cellular_network_status);
    snprintf(gps, sizeof(gps), "%s", cellular_gps_status);
    snprintf(sats, sizeof(sats), "%s", cellular_sat_status);
    memcpy(sats_copy, cellular_sats, sizeof(sats_copy));
    gnss_start_us = cellular_gnss_start_us;
    gnss_fix_us = cellular_gnss_fix_us;
    gnss_fix_valid = cellular_gnss_fix_valid;
    active = cellular_worker_active;
    monitor_active = cellular_cno_monitor_active;
    snprintf(running_title, sizeof(running_title), "%s",
             cellular_running_title);
    pthread_mutex_unlock(&cellular_lock);
    cellular_ttff_text(ttff, sizeof(ttff), gnss_start_us, gnss_fix_us,
                       gnss_fix_valid);

    if(cellular_status_label) {
        char text[224];

        snprintf(text, sizeof(text), "%s  EN=%s  %s",
                 exists ? NRF9151_UART_DEV : ui_tr("nRF9151 UART missing"),
                 NRF9151_EN_CONTROL_ENABLED ? "GPIO2" : "external",
                 active || monitor_active ? ui_tr("Running") : status);
        lv_label_set_text(cellular_status_label, text);
        lv_obj_set_style_text_color(cellular_status_label,
                                    lv_color_hex(exists ? 0x25C281 : 0xF5A524),
                                    0);
    }
    cellular_check_progress_refresh(active, status, running_title);
    if(cellular_link_label) {
        lv_label_set_text(cellular_link_label, link);
    }
    if(cellular_sim_label) {
        lv_label_set_text(cellular_sim_label, sim);
    }
    if(cellular_lte_label) {
        lv_label_set_text(cellular_lte_label, lte);
    }
    if(cellular_gnss_label) {
        lv_label_set_text(cellular_gnss_label, gnss);
    }
    if(cellular_imei_label) {
        lv_label_set_text(cellular_imei_label, imei);
    }
    if(cellular_network_label) {
        lv_label_set_text(cellular_network_label, network);
    }
    if(cellular_gps_label) {
        lv_label_set_text(cellular_gps_label, gps);
    }
    if(cellular_ttff_label) {
        lv_label_set_text(cellular_ttff_label, ttff);
        lv_obj_set_style_text_color(cellular_ttff_label,
                                    lv_color_hex(gnss_fix_valid ? 0x25C281 :
                                                 0xF5A524),
                                    0);
    }
    cellular_cn0_chart_refresh(sats_copy, NRF9151_MAX_SATS);
    if(cellular_sat_label) {
        lv_label_set_text(cellular_sat_label, sats);
    }
    if(cellular_cno_button) {
        lv_obj_t *label = lv_obj_get_child(cellular_cno_button, 0);

        if(label) {
            lv_label_set_text(label, ui_tr(monitor_active ? "Stop GNSS" :
                                           "GNSS"));
            lv_obj_set_style_text_color(label,
                                        lv_color_hex(monitor_active ?
                                                     0xEF4D5A : 0xF97316),
                                        0);
        }
    }
}

int ui_cellular_lte_signal_level(void)
{
    k230_nrf9151_status_t status;

    if(!k230_nrf9151_uart_present()) {
        return 0;
    }
    if(k230_nrf9151_read_status(&status, 120) != 0) {
        return 0;
    }
    if(!status.sim_ready) {
        return 0;
    }
    return status.lte_signal_level > 0 ? status.lte_signal_level :
           (status.pdp_active ? 4 :
            (status.packet_attached ? 3 :
             (status.lte_registered ? 2 : 0)));
}

static int cellular_open_uart(void)
{
    struct termios tio;
    int lock_fd;
    int fd;

    pthread_mutex_lock(&cellular_uart_owner_lock);
    if(cellular_uart_lock_fd >= 0) {
        pthread_mutex_unlock(&cellular_uart_owner_lock);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&cellular_uart_owner_lock);

    lock_fd = k230_nrf9151_acquire_uart("lte", 250);
    if(lock_fd < 0) {
        return -1;
    }
    cellular_configure_uart3_iomux();
    fd = open(NRF9151_UART_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        k230_nrf9151_release_uart(lock_fd);
        return -1;
    }
    memset(&tio, 0, sizeof(tio));
    if(tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, NRF9151_UART_BAUD);
        cfsetospeed(&tio, NRF9151_UART_BAUD);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~CRTSCTS;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    tcflush(fd, TCIOFLUSH);
    pthread_mutex_lock(&cellular_uart_owner_lock);
    cellular_uart_lock_fd = lock_fd;
    pthread_mutex_unlock(&cellular_uart_owner_lock);
    return fd;
}

static void cellular_close_uart(int fd)
{
    int lock_fd;

    if(fd >= 0) {
        close(fd);
    }

    pthread_mutex_lock(&cellular_uart_owner_lock);
    lock_fd = cellular_uart_lock_fd;
    cellular_uart_lock_fd = -1;
    pthread_mutex_unlock(&cellular_uart_owner_lock);

    if(lock_fd >= 0) {
        k230_nrf9151_release_uart(lock_fd);
    }
}

static void cellular_response_clean(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;

    if(!dst || dst_len == 0) {
        return;
    }
    if(!src) {
        dst[0] = '\0';
        return;
    }
    for(size_t i = 0; src[i] && j + 1U < dst_len; i++) {
        char c = src[i];

        if(c == '\r') {
            continue;
        }
        if(c == '\n') {
            if(j > 0 && dst[j - 1U] != '\n') {
                dst[j++] = '\n';
            }
            continue;
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static int cellular_response_has_ok(const char *resp)
{
    return resp && (strstr(resp, "\r\nOK\r\n") ||
                    strstr(resp, "\nOK\n") ||
                    strstr(resp, "\r\nOK\n") ||
                    strstr(resp, "\nOK\r\n") ||
                    strcmp(resp, "OK") == 0);
}

static int cellular_response_has_error(const char *resp)
{
    return resp && (strstr(resp, "ERROR") || strstr(resp, "+CME ERROR") ||
                    strstr(resp, "+CMS ERROR"));
}

static int cellular_exchange_fd(int fd, const char *cmd, char *resp,
                                size_t resp_len, uint64_t timeout_us)
{
    char tx[128];
    char clean[768];
    uint64_t deadline;
    size_t used = 0;
    int result = -1;

    if(resp_len > 0) {
        resp[0] = '\0';
    }
    if(!cmd || !cmd[0]) {
        return -1;
    }

    snprintf(tx, sizeof(tx), "%s\r\n", cmd);
    cellular_log_append("> %s", cmd);
    if(write(fd, tx, strlen(tx)) < 0) {
        cellular_log_append("< write failed: %s", strerror(errno));
        return -1;
    }
    tcdrain(fd);

    deadline = ui_monotonic_us() + timeout_us;
    while(ui_monotonic_us() < deadline) {
        fd_set rfds;
        struct timeval tv;
        char buf[160];
        ssize_t rd;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if(select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        rd = read(fd, buf, sizeof(buf) - 1);
        if(rd <= 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }
        buf[rd] = '\0';
        if(resp && resp_len > 1U && used + (size_t)rd + 1U < resp_len) {
            memcpy(resp + used, buf, (size_t)rd);
            used += (size_t)rd;
            resp[used] = '\0';
        }
        if(cellular_response_has_ok(resp) || cellular_response_has_error(resp)) {
            break;
        }
    }

    if(resp && resp[0]) {
        cellular_response_clean(resp, clean, sizeof(clean));
        cellular_log_append("< %s", clean);
        result = cellular_response_has_ok(resp) ? 0 :
                 cellular_response_has_error(resp) ? 1 : -1;
    } else {
        cellular_log_append("< timeout");
    }
    return result;
}

static int cellular_try_led_mode(int fd, int mode, const char *reason)
{
    char cmd[32];
    char resp[256];
    int rc;

    if(fd < 0) {
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "AT#XK230LED=%d", mode);
    rc = cellular_exchange_fd(fd, cmd, resp, sizeof(resp),
                              NRF9151_CMD_TIMEOUT_US);
    cellular_process_response_lines(resp);
    if(rc == 0) {
        cellular_log_append("GNSS LED %s mode=%d", reason ? reason : "set",
                            mode);
    } else {
        cellular_log_append("GNSS LED control unavailable rc=%d", rc);
    }
    return rc;
}

static void cellular_apply_led_auto_if_pending(int fd)
{
    int pending;

    pthread_mutex_lock(&cellular_lock);
    pending = cellular_led_auto_pending;
    if(pending) {
        cellular_led_auto_pending = 0;
    }
    pthread_mutex_unlock(&cellular_lock);

    if(pending) {
        cellular_try_led_mode(fd, 2, "fix");
    }
}

static int cellular_line_containing(const char *resp, const char *needle,
                                    char *out, size_t out_len)
{
    const char *p;

    if(!resp || !needle || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    p = strstr(resp, needle);
    if(!p) {
        return -1;
    }
    while(p > resp && p[-1] != '\n' && p[-1] != '\r') {
        p--;
    }
    for(size_t i = 0; p[i] && p[i] != '\n' && p[i] != '\r' &&
        i + 1U < out_len; i++) {
        out[i] = p[i];
        out[i + 1U] = '\0';
    }
    ui_trim_text(out);
    return out[0] ? 0 : -1;
}

static int cellular_parse_int_field(const char *s)
{
    if(!s || !s[0]) {
        return -1;
    }
    return atoi(s);
}

static int cellular_nmea_checksum_ok(const char *line)
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
    return (calc & 0xFFU) == (expect & 0xFFU);
}

static int cellular_nmea_split(char *body, char **fields, int max_fields)
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

static int cellular_nmea_coord_to_decimal(const char *value, const char *dir,
                                          char *out, size_t out_len)
{
    int deg_width;
    int degrees;
    double raw;
    double minutes;
    double decimal;
    char deg_str[4];

    if(!value || !value[0] || !dir || !dir[0] || !out || out_len == 0) {
        return -1;
    }
    deg_width = (dir[0] == 'N' || dir[0] == 'S') ? 2 : 3;
    if((int)strlen(value) <= deg_width) {
        return -1;
    }
    memset(deg_str, 0, sizeof(deg_str));
    memcpy(deg_str, value, (size_t)deg_width);
    degrees = atoi(deg_str);
    raw = strtod(value + deg_width, NULL);
    minutes = raw / 60.0;
    decimal = (double)degrees + minutes;
    if(dir[0] == 'S' || dir[0] == 'W') {
        decimal = -decimal;
    }
    snprintf(out, out_len, "%.6f", decimal);
    return 0;
}

static void cellular_sat_status_rebuild_locked(void)
{
    char text[NRF9151_SAT_TEXT_MAX];
    size_t used = 0;
    int count = 0;
    int best = -1;

    used += snprintf(text + used, sizeof(text) - used,
                     "GPS C/N0 quality\n");
    for(size_t i = 0; i < NRF9151_MAX_SATS && used + 1U < sizeof(text); i++) {
        const cellular_satellite_t *sat = &cellular_sats[i];
        char bar[16];
        int bars;

        if(!sat->valid) {
            continue;
        }
        bars = sat->cn0 > 0 ? sat->cn0 / 4 : 0;
        if(bars > 12) {
            bars = 12;
        }
        memset(bar, '#', (size_t)bars);
        bar[bars] = '\0';
        used += snprintf(text + used, sizeof(text) - used,
                         "%s%02d  el%02d az%03d  %2d dB-Hz  %-12s\n",
                         sat->talker, sat->prn, sat->elevation, sat->azimuth,
                         sat->cn0, bar);
        if(sat->cn0 > best) {
            best = sat->cn0;
        }
        count++;
    }
    if(count == 0) {
        snprintf(cellular_sat_status, sizeof(cellular_sat_status),
                 "%s", "No satellite data");
    } else {
        snprintf(cellular_sat_status, sizeof(cellular_sat_status),
                 "%s", text);
        snprintf(cellular_gnss_status, sizeof(cellular_gnss_status),
                 "%s", "OK");
    }
}

static void cellular_nmea_upsert_sat(const char *talker, int prn, int elevation,
                                     int azimuth, int cn0)
{
    cellular_satellite_t *slot = NULL;
    uint64_t now = ui_monotonic_us();

    if(!talker || prn <= 0) {
        return;
    }

    pthread_mutex_lock(&cellular_lock);
    for(size_t i = 0; i < NRF9151_MAX_SATS; i++) {
        if(cellular_sats[i].valid &&
           cellular_sats[i].prn == prn &&
           strcmp(cellular_sats[i].talker, talker) == 0) {
            slot = &cellular_sats[i];
            break;
        }
        if(!cellular_sats[i].valid && !slot) {
            slot = &cellular_sats[i];
        }
    }
    if(slot) {
        snprintf(slot->talker, sizeof(slot->talker), "%s", talker);
        slot->prn = prn;
        slot->elevation = elevation >= 0 ? elevation : 0;
        slot->azimuth = azimuth >= 0 ? azimuth : 0;
        slot->cn0 = cn0 >= 0 ? cn0 : 0;
        slot->last_seen_us = now;
        slot->valid = 1;
        cellular_sat_status_rebuild_locked();
    }
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_nmea_parse_gga(char **fields, int count)
{
    char lat[32] = "";
    char lon[32] = "";
    const char *utc = count > 1 ? fields[1] : "";
    const char *hdop = count > 8 ? fields[8] : "";
    int fix = count > 6 ? cellular_parse_int_field(fields[6]) : -1;
    int sats = count > 7 ? cellular_parse_int_field(fields[7]) : -1;
    int first_fix = 0;
    double ttff_s = 0.0;
    double lat_value = 0.0;
    double lon_value = 0.0;
    double alt_value = 0.0;
    int has_alt = 0;

    if(count > 5) {
        cellular_nmea_coord_to_decimal(fields[2], fields[3], lat, sizeof(lat));
        cellular_nmea_coord_to_decimal(fields[4], fields[5], lon, sizeof(lon));
    }
    if(lat[0] && lon[0]) {
        lat_value = strtod(lat, NULL);
        lon_value = strtod(lon, NULL);
    }
    if(count > 9 && fields[9] && fields[9][0]) {
        alt_value = strtod(fields[9], NULL);
        has_alt = isfinite(alt_value) ? 1 : 0;
    }

    pthread_mutex_lock(&cellular_lock);
    if(fix > 0 && lat[0] && lon[0]) {
        first_fix = cellular_gnss_note_fix_locked(&ttff_s);
        snprintf(cellular_gps_status, sizeof(cellular_gps_status),
                 "Fix %d  Sat %d  HDOP %s\nLat %s\nLon %s",
                 fix, sats, hdop[0] ? hdop : "--", lat, lon);
        snprintf(cellular_gnss_status, sizeof(cellular_gnss_status),
                 "%s", "OK");
    } else {
        snprintf(cellular_gps_status, sizeof(cellular_gps_status),
                 "No fix  Sat=%d  HDOP=%s\nUTC=%s",
                 sats, hdop[0] ? hdop : "--", utc[0] ? utc : "--");
        snprintf(cellular_gnss_status, sizeof(cellular_gnss_status),
                 "%s", "OK");
    }
    pthread_mutex_unlock(&cellular_lock);
    if(fix > 0 && lat[0] && lon[0]) {
        cellular_gnss_write_fix_cache(lat_value, lon_value, has_alt, alt_value,
                                      sats, "lte-gga");
    }
    if(first_fix) {
        cellular_log_append("GNSS first fix TTFF %.1fs", ttff_s);
    }
}

static void cellular_nmea_parse_rmc(char **fields, int count)
{
    char lat[32] = "";
    char lon[32] = "";
    const char *valid = count > 2 ? fields[2] : "";
    const char *speed = count > 7 ? fields[7] : "";
    int first_fix;
    double ttff_s = 0.0;
    double lat_value = 0.0;
    double lon_value = 0.0;

    if(count > 6) {
        cellular_nmea_coord_to_decimal(fields[3], fields[4], lat, sizeof(lat));
        cellular_nmea_coord_to_decimal(fields[5], fields[6], lon, sizeof(lon));
    }
    if(valid[0] != 'A' || !lat[0] || !lon[0]) {
        return;
    }
    lat_value = strtod(lat, NULL);
    lon_value = strtod(lon, NULL);

    pthread_mutex_lock(&cellular_lock);
    first_fix = cellular_gnss_note_fix_locked(&ttff_s);
    snprintf(cellular_gps_status, sizeof(cellular_gps_status),
             "RMC valid  Speed %s kn\nLat %s\nLon %s",
             speed[0] ? speed : "--", lat, lon);
    snprintf(cellular_gnss_status, sizeof(cellular_gnss_status),
             "%s", "OK");
    pthread_mutex_unlock(&cellular_lock);
    cellular_gnss_write_fix_cache(lat_value, lon_value, 0, 0.0, -1,
                                  "lte-rmc");
    if(first_fix) {
        cellular_log_append("GNSS first fix TTFF %.1fs", ttff_s);
    }
}

static void cellular_parse_gnsspos(const char *line)
{
    const char *p;
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    double acc = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    char datetime[64] = "";
    int parsed;
    int first_fix;
    double ttff_s = 0.0;

    if(!line) {
        return;
    }
    p = strchr(line, ':');
    if(!p) {
        return;
    }
    p = cellular_skip_spaces(p + 1);
    parsed = sscanf(p, "%lf,%lf,%lf,%lf,%lf,%lf,\"%63[^\"]\"",
                    &lat, &lon, &alt, &acc, &speed, &heading, datetime);
    if(parsed < 2) {
        cellular_log_append("GNSS POS parse failed: %.96s", line);
        return;
    }

    pthread_mutex_lock(&cellular_lock);
    first_fix = cellular_gnss_note_fix_locked(&ttff_s);
    snprintf(cellular_gps_status, sizeof(cellular_gps_status),
             "POS fix  Acc %.1fm\nLat %.6f\nLon %.6f",
             acc, lat, lon);
    snprintf(cellular_gnss_status, sizeof(cellular_gnss_status),
             "%s", "OK");
    pthread_mutex_unlock(&cellular_lock);
    cellular_gnss_write_fix_cache(lat, lon, parsed >= 3, alt, -1,
                                  "lte-xgnsspos");

    if(first_fix) {
        cellular_log_append("GNSS first fix TTFF %.1fs from XGNSSPOS", ttff_s);
    }
}

static void cellular_nmea_parse_gsv(const char *talker, char **fields,
                                    int count)
{
    int total_sats = count > 3 ? cellular_parse_int_field(fields[3]) : -1;

    for(int i = 4; i + 3 < count; i += 4) {
        int prn = cellular_parse_int_field(fields[i]);
        int elevation = cellular_parse_int_field(fields[i + 1]);
        int azimuth = cellular_parse_int_field(fields[i + 2]);
        int cn0 = cellular_parse_int_field(fields[i + 3]);

        if(prn > 0) {
            cellular_nmea_upsert_sat(talker, prn, elevation, azimuth, cn0);
        }
    }
    if(total_sats >= 0) {
        pthread_mutex_lock(&cellular_lock);
        if(strcmp(cellular_sat_status, "No satellite data") == 0) {
            snprintf(cellular_sat_status, sizeof(cellular_sat_status),
                     "GSV reports %d satellites, no C/N0 yet", total_sats);
        }
        pthread_mutex_unlock(&cellular_lock);
    }
}

static void cellular_nmea_parse_sentence(const char *line)
{
    char body[NRF9151_NMEA_LINE_MAX];
    char talker[3] = "";
    char type[4] = "";
    char *fields[32];
    const char *star;
    size_t len;
    int count;

    if(!line || line[0] != '$') {
        return;
    }
    if(!cellular_nmea_checksum_ok(line)) {
        cellular_log_append("NMEA checksum failed: %.80s", line);
        return;
    }

    star = strchr(line, '*');
    len = star ? (size_t)(star - line - 1) : strlen(line + 1);
    if(len >= sizeof(body)) {
        len = sizeof(body) - 1U;
    }
    memcpy(body, line + 1, len);
    body[len] = '\0';
    count = cellular_nmea_split(body, fields, (int)(sizeof(fields) /
                                  sizeof(fields[0])));
    if(count <= 0 || strlen(fields[0]) < 5U) {
        return;
    }
    snprintf(talker, sizeof(talker), "%.2s", fields[0]);
    snprintf(type, sizeof(type), "%.3s", fields[0] + strlen(fields[0]) - 3U);

    if(strcmp(type, "GGA") == 0) {
        cellular_nmea_parse_gga(fields, count);
    } else if(strcmp(type, "RMC") == 0) {
        cellular_nmea_parse_rmc(fields, count);
    } else if(strcmp(type, "GSV") == 0) {
        cellular_nmea_parse_gsv(talker, fields, count);
    }
}

static const char *cellular_skip_spaces(const char *s)
{
    while(s && *s && isspace((unsigned char)*s)) {
        s++;
    }
    return s ? s : "";
}

static int cellular_line_has_nmea_prefix(const char *line)
{
    return line && (strncmp(line, "$GP", 3) == 0 ||
                    strncmp(line, "$GN", 3) == 0 ||
                    strncmp(line, "$GA", 3) == 0 ||
                    strncmp(line, "$GB", 3) == 0 ||
                    strncmp(line, "$BD", 3) == 0);
}

static void cellular_note_nmea_sentence(const char *nmea)
{
    if(!nmea || !cellular_line_has_nmea_prefix(nmea)) {
        return;
    }

    pthread_mutex_lock(&cellular_lock);
    cellular_nmea_count++;
    snprintf(cellular_last_nmea, sizeof(cellular_last_nmea), "%s", nmea);
    pthread_mutex_unlock(&cellular_lock);
    cellular_nmea_parse_sentence(nmea);
}

static void cellular_process_uart_line(const char *line)
{
    char clean[NRF9151_UART_LINE_MAX];
    const char *nmea = NULL;

    if(!line) {
        return;
    }
    snprintf(clean, sizeof(clean), "%s", line);
    ui_trim_text(clean);
    if(!clean[0]) {
        return;
    }

    cellular_log_append("< %s", clean);

    pthread_mutex_lock(&cellular_lock);
    cellular_urc_count++;
    cellular_last_uart_us = ui_monotonic_us();
    snprintf(cellular_last_urc, sizeof(cellular_last_urc), "%s", clean);
    pthread_mutex_unlock(&cellular_lock);

    cellular_lte_update_from_line(clean, 0);

    if(strncmp(clean, "#XGNSSNMEA:", 11) == 0) {
        nmea = cellular_skip_spaces(clean + 11);
    } else if(cellular_line_has_nmea_prefix(clean)) {
        nmea = clean;
    }

    if(nmea && cellular_line_has_nmea_prefix(nmea)) {
        cellular_note_nmea_sentence(nmea);
        return;
    }

    if(strncmp(clean, "#XGNSSPOS:", 10) == 0) {
        cellular_parse_gnsspos(clean);
        return;
    }

    if(strncmp(clean, "#XGNSS:", 7) == 0 ||
       strncmp(clean, "GNSS:", 5) == 0 ||
       strncmp(clean, "#XNMEA:", 7) == 0 ||
       strncmp(clean, "#XGPS:", 6) == 0) {
        int gnss_active = cellular_response_gnss_active(clean);

        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             "%s", gnss_active == 0 ? "FAIL" : "OK");
    }
}

static void cellular_uart_line_feed(char *line, size_t *used, const char *buf,
                                    size_t len)
{
    if(!line || !used || !buf) {
        return;
    }
    for(size_t i = 0; i < len; i++) {
        char c = buf[i];

        if(c == '\r') {
            continue;
        }
        if(c == '\n') {
            line[*used] = '\0';
            cellular_process_uart_line(line);
            *used = 0;
            continue;
        }
        if(*used + 1U >= NRF9151_UART_LINE_MAX) {
            line[*used] = '\0';
            cellular_process_uart_line(line);
            *used = 0;
        }
        line[(*used)++] = c;
    }
}

static void cellular_process_response_line(const char *line)
{
    char clean[NRF9151_UART_LINE_MAX];
    const char *nmea = NULL;

    if(!line) {
        return;
    }
    snprintf(clean, sizeof(clean), "%s", line);
    ui_trim_text(clean);
    if(!clean[0] || strcmp(clean, "OK") == 0 || strcmp(clean, "ERROR") == 0) {
        return;
    }

    if(strncmp(clean, "#XGNSSNMEA:", 11) == 0) {
        nmea = cellular_skip_spaces(clean + 11);
    } else if(cellular_line_has_nmea_prefix(clean)) {
        nmea = clean;
    }

    if(nmea && cellular_line_has_nmea_prefix(nmea)) {
        cellular_note_nmea_sentence(nmea);
        return;
    }

    if(strncmp(clean, "#XGNSSPOS:", 10) == 0) {
        cellular_parse_gnsspos(clean);
        return;
    }

    if(strncmp(clean, "#XGNSS:", 7) == 0 ||
       strncmp(clean, "GNSS:", 5) == 0 ||
       strncmp(clean, "#XNMEA:", 7) == 0 ||
       strncmp(clean, "#XGPS:", 6) == 0) {
        int gnss_active = cellular_response_gnss_active(clean);

        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             "%s", gnss_active == 0 ? "FAIL" : "OK");
    }
}

static void cellular_process_response_lines(const char *resp)
{
    char line[NRF9151_UART_LINE_MAX];
    size_t used = 0;

    if(!resp) {
        return;
    }
    for(size_t i = 0; resp[i]; i++) {
        char c = resp[i];

        if(c == '\r') {
            continue;
        }
        if(c == '\n') {
            line[used] = '\0';
            cellular_process_response_line(line);
            used = 0;
            continue;
        }
        if(used + 1U >= sizeof(line)) {
            line[used] = '\0';
            cellular_process_response_line(line);
            used = 0;
        }
        line[used++] = c;
    }
    if(used > 0U) {
        line[used] = '\0';
        cellular_process_response_line(line);
    }
}

static int cellular_response_gnss_active(const char *resp)
{
    char line[160];
    const char *p;

    if(cellular_line_containing(resp, "#XGNSS:", line, sizeof(line)) != 0 &&
       cellular_line_containing(resp, "GNSS:", line, sizeof(line)) != 0) {
        return -1;
    }
    p = strchr(line, ':');
    if(!p) {
        return -1;
    }
    p = cellular_skip_spaces(p + 1);
    return atoi(p) > 0;
}

static int cellular_xsim_ready_line(const char *line)
{
    const char *p = strchr(line, ':');

    if(!p) {
        return 0;
    }
    p = cellular_skip_spaces(p + 1);
    return atoi(p) > 0;
}

static int cellular_cpin_ready_line(const char *line)
{
    const char *p = strchr(line, ':');

    if(!p) {
        return 0;
    }
    p = cellular_skip_spaces(p + 1);
    return strncmp(p, "READY", 5) == 0 || strncmp(p, "SIM PIN", 7) == 0 ||
           strncmp(p, "SIM PUK", 7) == 0;
}

static int cellular_iccid_line_valid(const char *line)
{
    const char *p = strchr(line, ':');

    if(!p) {
        return 0;
    }
    p = cellular_skip_spaces(p + 1);
    while(*p) {
        if(isdigit((unsigned char)*p)) {
            return 1;
        }
        p++;
    }
    return 0;
}

static int cellular_response_has_imsi(const char *resp)
{
    const char *p = resp;

    while(p && *p) {
        const char *line = p;
        size_t digits = 0;

        while(*p && *p != '\n' && *p != '\r') {
            p++;
        }
        for(const char *q = line; q < p; q++) {
            if(isdigit((unsigned char)*q)) {
                digits++;
            } else if(!isspace((unsigned char)*q)) {
                digits = 0;
                break;
            }
        }
        if(digits >= 5U) {
            return 1;
        }
        while(*p == '\n' || *p == '\r') {
            p++;
        }
    }
    return 0;
}

static int cellular_response_numeric_line(const char *resp, char *out,
                                          size_t out_len, size_t min_digits,
                                          size_t max_digits)
{
    const char *p = resp;

    if(!resp || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    while(p && *p) {
        const char *line = p;
        char digits[32];
        size_t count = 0;
        int invalid = 0;

        while(*p && *p != '\n' && *p != '\r') {
            p++;
        }
        for(const char *q = line; q < p; q++) {
            if(isdigit((unsigned char)*q)) {
                if(count + 1U < sizeof(digits)) {
                    digits[count++] = *q;
                }
            } else if(!isspace((unsigned char)*q)) {
                invalid = 1;
                break;
            }
        }
        if(!invalid && count >= min_digits && count <= max_digits) {
            size_t copy = count;

            if(copy >= out_len) {
                copy = out_len - 1U;
            }
            memcpy(out, digits, copy);
            out[copy] = '\0';
            return 0;
        }
        while(*p == '\n' || *p == '\r') {
            p++;
        }
    }
    return -1;
}

static int cellular_crsm_success_line(const char *line)
{
    const char *p = strchr(line, ':');
    int sw1;

    if(!p) {
        return 0;
    }
    p = cellular_skip_spaces(p + 1);
    sw1 = atoi(p);
    return sw1 == 144 || sw1 == 145;
}

static void cellular_lte_sim_fault_set(int fault)
{
    pthread_mutex_lock(&cellular_lock);
    cellular_lte_sim_fault_seen = fault ? 1 : 0;
    pthread_mutex_unlock(&cellular_lock);
}

static int cellular_lte_sim_fault_snapshot(void)
{
    int fault;

    pthread_mutex_lock(&cellular_lock);
    fault = cellular_lte_sim_fault_seen;
    pthread_mutex_unlock(&cellular_lock);
    return fault;
}

static int cellular_cereg_stat_from_line(const char *line)
{
    const char *p = strchr(line, ':');
    char *end = NULL;
    long first;
    long second = -1;

    if(!p) {
        return -1;
    }
    p = cellular_skip_spaces(p + 1);
    errno = 0;
    first = strtol(p, &end, 10);
    if(errno != 0 || end == p) {
        return -1;
    }
    p = cellular_skip_spaces(end);
    if(*p == ',') {
        p = cellular_skip_spaces(p + 1);
        errno = 0;
        second = strtol(p, &end, 10);
        if(errno == 0 && end != p) {
            return (int)second;
        }
    }
    return (int)first;
}

static const char *cellular_cereg_stat_text(int stat)
{
    switch(stat) {
    case 0:
        return "Not registered";
    case 1:
        return "Registered home";
    case 2:
        return "Searching";
    case 3:
        return "Registration denied";
    case 4:
        return "Unknown";
    case 5:
        return "Registered roaming";
    case 90:
        return "SIM/network error";
    default:
        return NULL;
    }
}

static int cellular_line_first_quoted(const char *line, char *out,
                                      size_t out_len)
{
    const char *start;
    const char *end;
    size_t len;

    if(!line || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    start = strchr(line, '"');
    if(!start) {
        return -1;
    }
    start++;
    end = strchr(start, '"');
    if(!end || end <= start) {
        return -1;
    }
    len = (size_t)(end - start);
    if(len >= out_len) {
        len = out_len - 1U;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out[0] ? 0 : -1;
}

static int cellular_line_cgpaddr_ip(const char *line, char *out,
                                    size_t out_len)
{
    const char *p;
    size_t len = 0;

    if(!line || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if(cellular_line_first_quoted(line, out, out_len) == 0) {
        return strcmp(out, "0.0.0.0") == 0 ? -1 : 0;
    }
    p = strchr(line, ':');
    if(!p) {
        return -1;
    }
    p = strchr(p, ',');
    if(!p) {
        return -1;
    }
    p = cellular_skip_spaces(p + 1);
    while(p[len] && p[len] != ',' && p[len] != '\r' && p[len] != '\n' &&
          !isspace((unsigned char)p[len])) {
        len++;
    }
    if(len == 0 || len >= out_len) {
        return -1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return strcmp(out, "0.0.0.0") == 0 ? -1 : 0;
}

static int cellular_line_cops_operator(const char *line, char *out,
                                       size_t out_len)
{
    const char *p;
    const char *start;
    const char *end;
    size_t len = 0;

    if(!line || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if(cellular_line_first_quoted(line, out, out_len) == 0) {
        return 0;
    }
    p = strchr(line, ':');
    if(!p) {
        return -1;
    }
    start = p + 1;
    end = start + strlen(start);
    while(start < end && (*start == ' ' || *start == ',')) {
        start++;
    }
    while(start < end && end[-1] && (end[-1] == '\r' || end[-1] == '\n' ||
          isspace((unsigned char)end[-1]))) {
        end--;
    }
    if(end <= start) {
        return -1;
    }
    len = (size_t)(end - start);
    if(len >= out_len) {
        len = out_len - 1U;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out[0] ? 0 : -1;
}

static void cellular_lte_update_from_line(const char *line, int rc)
{
    char ip[64];
    char op[80];
    const char *p;
    int value;

    if(!line) {
        return;
    }
    if(strncmp(line, "+CEREG:", 7) == 0) {
        int stat = cellular_cereg_stat_from_line(line);
        const char *text = cellular_cereg_stat_text(stat);

        if(stat == 90 && cellular_lte_sim_fault_snapshot()) {
            cellular_set_summary(cellular_lte_status,
                                 sizeof(cellular_lte_status),
                                 "SIM not detected");
        } else if(rc == 0 && text) {
            cellular_set_summary(cellular_lte_status,
                                 sizeof(cellular_lte_status), "%s", text);
        } else if(stat >= 0) {
            cellular_set_summary(cellular_lte_status,
                                 sizeof(cellular_lte_status),
                                 "CEREG %d", stat);
        } else {
            cellular_set_summary(cellular_lte_status,
                                 sizeof(cellular_lte_status),
                                 rc == 0 ? "CEREG OK" : "CEREG fail");
        }
        return;
    }
    if(strncmp(line, "+CGATT:", 8) == 0) {
        p = strchr(line, ':');
        value = p ? atoi(cellular_skip_spaces(p + 1)) : 0;
        cellular_set_summary(cellular_lte_status,
                             sizeof(cellular_lte_status),
                             value ? "Packet attached" : "Not attached");
        return;
    }
    if(strncmp(line, "+CGACT:", 8) == 0) {
        p = strrchr(line, ',');
        value = p ? atoi(cellular_skip_spaces(p + 1)) : 0;
        if(value) {
            cellular_set_summary(cellular_lte_status,
                                 sizeof(cellular_lte_status), "PDP active");
        }
        return;
    }
    if(strncmp(line, "+CGPADDR:", 9) == 0) {
        if(cellular_line_cgpaddr_ip(line, ip, sizeof(ip)) == 0) {
            cellular_set_ip(ip);
            cellular_set_summary(cellular_lte_status,
                                 sizeof(cellular_lte_status), "IP %s", ip);
        }
        return;
    }
    if(strncmp(line, "+COPS:", 6) == 0) {
        if(cellular_line_cops_operator(line, op, sizeof(op)) == 0) {
            cellular_set_operator(op);
        }
        return;
    }
    if(strncmp(line, "#XPING:", 7) == 0) {
        cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                             strstr(line, "average") ? "Ping OK" :
                             "Ping reply");
        return;
    }
}

static void cellular_sim_mark_positive(void)
{
    pthread_mutex_lock(&cellular_lock);
    cellular_sim_positive_seen = 1;
    snprintf(cellular_sim_status, sizeof(cellular_sim_status), "%s", "OK");
    pthread_mutex_unlock(&cellular_lock);
}

static int cellular_sim_positive_snapshot(void)
{
    int positive;

    pthread_mutex_lock(&cellular_lock);
    positive = cellular_sim_positive_seen;
    pthread_mutex_unlock(&cellular_lock);
    return positive;
}

static void cellular_sim_mark_fail_if_no_positive(void)
{
    pthread_mutex_lock(&cellular_lock);
    if(!cellular_sim_positive_seen) {
        snprintf(cellular_sim_status, sizeof(cellular_sim_status), "%s", "FAIL");
    }
    pthread_mutex_unlock(&cellular_lock);
}

static void cellular_set_action_result(cellular_action_t action, int ok)
{
    const char *text = ok ? "OK" : "FAIL";

    switch(action) {
    case CELLULAR_ACTION_LINK:
        cellular_set_summary(cellular_link_status, sizeof(cellular_link_status),
                             "%s", text);
        break;
    case CELLULAR_ACTION_SIM:
        cellular_set_summary(cellular_sim_status, sizeof(cellular_sim_status),
                             "%s", text);
        break;
    case CELLULAR_ACTION_LTE_STATUS:
    case CELLULAR_ACTION_LTE_GNSS_MODE:
        cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                             "%s", text);
        break;
    case CELLULAR_ACTION_GNSS_START:
    case CELLULAR_ACTION_GNSS_STATUS:
    case CELLULAR_ACTION_GNSS_NMEA:
    case CELLULAR_ACTION_GNSS_STOP:
    case CELLULAR_ACTION_FULL_TEST:
        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             "%s", text);
        break;
    case CELLULAR_ACTION_HTTP_GET:
    case CELLULAR_ACTION_HTTP_POST:
    case CELLULAR_ACTION_MQTT_TEST:
        cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                             "%s", text);
        break;
    }
}

static void cellular_update_from_response(cellular_action_t action,
                                          const char *cmd, const char *resp,
                                          int rc)
{
    char line[160];

    cellular_process_response_lines(resp);

    if(rc == 0 && strcmp(cmd, "AT") == 0) {
        cellular_set_summary(cellular_link_status, sizeof(cellular_link_status),
                             "OK");
    } else if(rc != 0 && strcmp(cmd, "AT") == 0) {
        cellular_set_summary(cellular_link_status, sizeof(cellular_link_status),
                             "FAIL");
    }

    if(rc == 0 && strcmp(cmd, "AT+CGSN") == 0) {
        char imei[32];

        if(cellular_response_numeric_line(resp, imei, sizeof(imei), 14, 17) == 0) {
            cellular_set_imei(imei);
        }
        if(action == CELLULAR_ACTION_LINK ||
           action == CELLULAR_ACTION_FULL_TEST) {
            cellular_set_summary(cellular_link_status,
                                 sizeof(cellular_link_status), "OK");
        }
    }
    if(rc == 0 && strcmp(cmd, "AT+CGMR") == 0) {
        char version[96];

        if(cellular_line_containing(resp, "mfw_", version,
                                    sizeof(version)) == 0) {
            cellular_set_summary(cellular_link_status,
                                 sizeof(cellular_link_status),
                                 "%s", version);
        }
    }

    if(cellular_line_containing(resp, "%XSIM:", line, sizeof(line)) == 0) {
        if(rc == 0 && cellular_xsim_ready_line(line)) {
            cellular_sim_mark_positive();
            if(action == CELLULAR_ACTION_LTE_STATUS ||
               action == CELLULAR_ACTION_LTE_GNSS_MODE) {
                cellular_lte_sim_fault_set(0);
            }
        } else {
            cellular_sim_mark_fail_if_no_positive();
            if(action == CELLULAR_ACTION_LTE_STATUS ||
               action == CELLULAR_ACTION_LTE_GNSS_MODE) {
                cellular_lte_sim_fault_set(1);
                cellular_set_summary(cellular_lte_status,
                                     sizeof(cellular_lte_status),
                                     "SIM not detected");
            }
        }
    }
    if(cellular_line_containing(resp, "+CPIN:", line, sizeof(line)) == 0) {
        if(rc == 0 && cellular_cpin_ready_line(line)) {
            cellular_sim_mark_positive();
            if(action == CELLULAR_ACTION_LTE_STATUS ||
               action == CELLULAR_ACTION_LTE_GNSS_MODE) {
                cellular_lte_sim_fault_set(0);
            }
        } else {
            cellular_sim_mark_fail_if_no_positive();
            if(action == CELLULAR_ACTION_LTE_STATUS ||
               action == CELLULAR_ACTION_LTE_GNSS_MODE) {
                cellular_lte_sim_fault_set(1);
                cellular_set_summary(cellular_lte_status,
                                     sizeof(cellular_lte_status),
                                     "SIM not ready");
            }
        }
    }
    if(cellular_line_containing(resp, "%XICCID:", line, sizeof(line)) == 0) {
        if(rc == 0 && cellular_iccid_line_valid(line)) {
            cellular_sim_mark_positive();
        } else {
            cellular_sim_mark_fail_if_no_positive();
        }
    }
    if((action == CELLULAR_ACTION_SIM || action == CELLULAR_ACTION_FULL_TEST) &&
       rc == 0 && strcmp(cmd, "AT+CIMI") == 0) {
        if(cellular_response_has_imsi(resp)) {
            cellular_sim_mark_positive();
        }
    }
    if(cellular_line_containing(resp, "+CRSM:", line, sizeof(line)) == 0) {
        if(rc == 0 && cellular_crsm_success_line(line)) {
            cellular_sim_mark_positive();
        } else {
            cellular_sim_mark_fail_if_no_positive();
        }
    }

    if(cellular_line_containing(resp, "+CEREG:", line, sizeof(line)) == 0) {
        cellular_lte_update_from_line(line, rc);
    }
    if(cellular_line_containing(resp, "+CGATT:", line, sizeof(line)) == 0) {
        cellular_lte_update_from_line(line, rc);
    }
    if(cellular_line_containing(resp, "+CGACT:", line, sizeof(line)) == 0) {
        cellular_lte_update_from_line(line, rc);
    }
    if(cellular_line_containing(resp, "+CGPADDR:", line, sizeof(line)) == 0) {
        cellular_lte_update_from_line(line, rc);
    }
    if(cellular_line_containing(resp, "+COPS:", line, sizeof(line)) == 0) {
        cellular_lte_update_from_line(line, rc);
    }
    if(cellular_line_containing(resp, "#XPING:", line, sizeof(line)) == 0) {
        cellular_lte_update_from_line(line, rc);
    }

    if(cellular_line_containing(resp, "#XGNSS:", line, sizeof(line)) == 0 ||
       cellular_line_containing(resp, "GNSS:", line, sizeof(line)) == 0) {
        int gnss_active = cellular_response_gnss_active(line);

        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             rc == 0 && gnss_active != 0 ? "OK" : "FAIL");
    } else if(cellular_line_containing(resp, "#XGPS:", line, sizeof(line)) == 0 ||
              cellular_line_containing(resp, "#XNMEA:", line,
                                       sizeof(line)) == 0) {
        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             rc == 0 ? "OK" : "FAIL");
    }
    if(rc != 0 && strstr(cmd, "AT#X")) {
        if(action == CELLULAR_ACTION_FULL_TEST &&
           strcmp(cmd, "AT#XGNSS=1,0,0,0") == 0) {
            cellular_set_summary(cellular_gnss_status,
                                 sizeof(cellular_gnss_status),
                                 "FAIL");
        } else {
            cellular_set_summary(cellular_gnss_status,
                                 sizeof(cellular_gnss_status),
                                 "FAIL");
        }
    }
    if((action == CELLULAR_ACTION_GNSS_START ||
        action == CELLULAR_ACTION_FULL_TEST) &&
       strcmp(cmd, "AT#XGNSS=1,0,0,0") == 0 && rc == 0) {
        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             "OK");
    }
    if(action == CELLULAR_ACTION_GNSS_STOP && strcmp(cmd, "AT#XGNSS=0") == 0 &&
       rc == 0) {
        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             "OK");
    }
    if(rc != 0 &&
       (action == CELLULAR_ACTION_SIM || action == CELLULAR_ACTION_FULL_TEST) &&
       (strstr(cmd, "XSIM") || strcmp(cmd, "AT+CPIN?") == 0 ||
        strcmp(cmd, "AT%XICCID") == 0 || strcmp(cmd, "AT+CIMI") == 0 ||
        strstr(cmd, "AT+CRSM") == cmd)) {
        cellular_sim_mark_fail_if_no_positive();
    }
    if(rc != 0 &&
       (action == CELLULAR_ACTION_LTE_STATUS ||
        action == CELLULAR_ACTION_LTE_GNSS_MODE) &&
       (strstr(cmd, "XSIM") || strcmp(cmd, "AT+CPIN?") == 0 ||
        strcmp(cmd, "AT%XICCID") == 0 || strcmp(cmd, "AT+CIMI") == 0)) {
        cellular_lte_sim_fault_set(1);
        cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                             strcmp(cmd, "AT+CPIN?") == 0 ?
                             "SIM not ready" : "SIM not detected");
    }
}

static void cellular_read_unsolicited(int fd, unsigned int seconds)
{
    uint64_t deadline = ui_monotonic_us() + (uint64_t)seconds * 1000000ULL;
    char uart_line[NRF9151_UART_LINE_MAX];
    unsigned int urc_before;
    unsigned int nmea_before;
    unsigned int urc_after;
    unsigned int nmea_after;
    size_t uart_used = 0;
    int byte_seen = 0;

    pthread_mutex_lock(&cellular_lock);
    urc_before = cellular_urc_count;
    nmea_before = cellular_nmea_count;
    pthread_mutex_unlock(&cellular_lock);
    cellular_log_append("Reading NMEA/URC UART data for %us", seconds);
    while(ui_monotonic_us() < deadline) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        if(select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        rd = read(fd, buf, sizeof(buf) - 1);
        if(rd <= 0) {
            continue;
        }
        byte_seen = 1;
        cellular_uart_line_feed(uart_line, &uart_used, buf, (size_t)rd);
        cellular_apply_led_auto_if_pending(fd);
    }
    if(uart_used > 0U) {
        uart_line[uart_used] = '\0';
        cellular_process_uart_line(uart_line);
        cellular_apply_led_auto_if_pending(fd);
    }

    pthread_mutex_lock(&cellular_lock);
    urc_after = cellular_urc_count;
    nmea_after = cellular_nmea_count;
    pthread_mutex_unlock(&cellular_lock);

    if(!byte_seen) {
        cellular_log_append("< no UART data");
        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             "FAIL");
    } else {
        cellular_log_append("NMEA read summary: urc=%u nmea=%u",
                            urc_after - urc_before,
                            nmea_after - nmea_before);
        cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                             nmea_after > nmea_before ? "OK" : "FAIL");
    }
}

static void cellular_ping_line_process(char *line, int *ping_seen,
                                       int *average_seen, int *error_seen)
{
    char clean[NRF9151_UART_LINE_MAX];

    if(!line) {
        return;
    }
    snprintf(clean, sizeof(clean), "%s", line);
    ui_trim_text(clean);
    if(!clean[0]) {
        return;
    }
    if(strncmp(clean, "#XPING:", 7) == 0) {
        if(ping_seen) {
            *ping_seen = 1;
        }
        if(strstr(clean, "average") && average_seen) {
            *average_seen = 1;
        }
    } else if(strstr(clean, "ERROR") && error_seen) {
        *error_seen = 1;
    }
    cellular_process_uart_line(clean);
}

static int cellular_read_ping_result(int fd, unsigned int seconds)
{
    uint64_t deadline = ui_monotonic_us() + (uint64_t)seconds * 1000000ULL;
    char uart_line[NRF9151_UART_LINE_MAX];
    size_t uart_used = 0;
    int ping_seen = 0;
    int average_seen = 0;
    int error_seen = 0;

    cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                         "Pinging");
    cellular_log_append("Reading XPING result for %us", seconds);
    while(ui_monotonic_us() < deadline && !average_seen && !error_seen) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        if(select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        rd = read(fd, buf, sizeof(buf));
        if(rd <= 0) {
            continue;
        }
        for(ssize_t i = 0; i < rd; i++) {
            char c = buf[i];

            if(c == '\r') {
                continue;
            }
            if(c == '\n') {
                uart_line[uart_used] = '\0';
                cellular_ping_line_process(uart_line, &ping_seen,
                                           &average_seen, &error_seen);
                uart_used = 0;
                if(average_seen || error_seen) {
                    break;
                }
                continue;
            }
            if(uart_used + 1U >= sizeof(uart_line)) {
                uart_line[uart_used] = '\0';
                cellular_ping_line_process(uart_line, &ping_seen,
                                           &average_seen, &error_seen);
                uart_used = 0;
            }
            uart_line[uart_used++] = c;
        }
    }
    if(uart_used > 0U) {
        uart_line[uart_used] = '\0';
        cellular_ping_line_process(uart_line, &ping_seen, &average_seen,
                                   &error_seen);
    }

    if(average_seen) {
        cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                             "Ping OK");
        return 0;
    }
    cellular_set_summary(cellular_lte_status, sizeof(cellular_lte_status),
                         ping_seen ? "Ping incomplete" : "Ping timeout");
    return 1;
}

static int cellular_run_command_list(int fd, cellular_action_t action,
                                     const char *const *cmds, size_t count)
{
    int failures = 0;

    for(size_t i = 0; i < count; i++) {
        char resp[1024];
        int is_ping = strncmp(cmds[i], "AT#XPING", 8) == 0;
        int rc = cellular_exchange_fd(fd, cmds[i], resp, sizeof(resp),
                                      NRF9151_CMD_TIMEOUT_US);

        cellular_update_from_response(action, cmds[i], resp, rc);
        cellular_apply_led_auto_if_pending(fd);
        if(rc != 0) {
            failures++;
        } else if(is_ping &&
                  cellular_read_ping_result(fd,
                                            NRF9151_LTE_PING_READ_SECONDS) != 0) {
            failures++;
        }
        usleep(120000);
    }
    return failures;
}

static int cellular_run_gnss_start(int fd, int reset_first)
{
    static const char *const setup_cmds[] = {
        "AT+CFUN=1",
        "AT%XSYSTEMMODE?",
        "AT#XNMEA=1",
    };
    char resp[1024];
    int rc;
    int failures = 0;

    if(reset_first) {
        cellular_log_append("GNSS cleanup before start");
        rc = cellular_exchange_fd(fd, "AT#XGNSS=0", resp, sizeof(resp),
                                  NRF9151_CMD_TIMEOUT_US);
        cellular_update_from_response(CELLULAR_ACTION_GNSS_STOP,
                                      "AT#XGNSS=0", resp, rc);
        rc = cellular_exchange_fd(fd, "AT#XNMEA=0", resp, sizeof(resp),
                                  NRF9151_CMD_TIMEOUT_US);
        cellular_update_from_response(CELLULAR_ACTION_GNSS_STOP,
                                      "AT#XNMEA=0", resp, rc);
    } else {
        rc = cellular_exchange_fd(fd, "AT#XGNSS?", resp, sizeof(resp),
                                  NRF9151_CMD_TIMEOUT_US);
        cellular_update_from_response(CELLULAR_ACTION_GNSS_STATUS,
                                      "AT#XGNSS?", resp, rc);
        if(rc == 0 && cellular_response_gnss_active(resp) > 0) {
            rc = cellular_exchange_fd(fd, "AT#XNMEA=1", resp, sizeof(resp),
                                      NRF9151_CMD_TIMEOUT_US);
            cellular_update_from_response(CELLULAR_ACTION_GNSS_START,
                                          "AT#XNMEA=1", resp, rc);
            if(rc == 0) {
                cellular_try_led_mode(fd, 1, "search");
            }
            cellular_set_summary(cellular_gnss_status,
                                 sizeof(cellular_gnss_status),
                                 rc == 0 ? "OK" : "FAIL");
            cellular_log_append("GNSS already running; start skipped");
            return rc == 0 ? 0 : 1;
        }
    }

    failures += cellular_run_command_list(fd, CELLULAR_ACTION_GNSS_START,
                                          setup_cmds,
                                          sizeof(setup_cmds) /
                                          sizeof(setup_cmds[0]));
    pthread_mutex_lock(&cellular_lock);
    cellular_gnss_session_start_locked();
    pthread_mutex_unlock(&cellular_lock);
    cellular_log_append("GNSS TTFF timer reset");
    cellular_try_led_mode(fd, 1, "search");
    rc = cellular_exchange_fd(fd, "AT#XGNSS=1,0,0,0", resp, sizeof(resp),
                              NRF9151_CMD_TIMEOUT_US);
    cellular_update_from_response(CELLULAR_ACTION_GNSS_START,
                                  "AT#XGNSS=1,0,0,0", resp, rc);
    if(rc != 0) {
        char status[1024];
        int status_rc = cellular_exchange_fd(fd, "AT#XGNSS?", status,
                                             sizeof(status),
                                             NRF9151_CMD_TIMEOUT_US);

        cellular_update_from_response(CELLULAR_ACTION_GNSS_STATUS,
                                      "AT#XGNSS?", status, status_rc);
        if(status_rc == 0 && cellular_response_gnss_active(status) > 0) {
            cellular_log_append("GNSS start returned ERROR but status is active");
            cellular_set_summary(cellular_gnss_status,
                                 sizeof(cellular_gnss_status), "OK");
            return failures;
        }
        failures++;
    }
    cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                         failures == 0 ? "OK" : "FAIL");
    return failures;
}

static int cellular_run_gnss_start_lte_mode(int fd, int reset_first)
{
    char resp[1024];
    int rc;
    int failures = 0;

    if(reset_first) {
        cellular_log_append("GNSS cleanup before LTE+GNSS start");
        rc = cellular_exchange_fd(fd, "AT#XGNSS=0", resp, sizeof(resp),
                                  NRF9151_CMD_TIMEOUT_US);
        cellular_update_from_response(CELLULAR_ACTION_GNSS_STOP,
                                      "AT#XGNSS=0", resp, rc);
        rc = cellular_exchange_fd(fd, "AT#XNMEA=0", resp, sizeof(resp),
                                  NRF9151_CMD_TIMEOUT_US);
        cellular_update_from_response(CELLULAR_ACTION_GNSS_STOP,
                                      "AT#XNMEA=0", resp, rc);
    } else {
        rc = cellular_exchange_fd(fd, "AT#XGNSS?", resp, sizeof(resp),
                                  NRF9151_CMD_TIMEOUT_US);
        cellular_update_from_response(CELLULAR_ACTION_GNSS_STATUS,
                                      "AT#XGNSS?", resp, rc);
        if(rc == 0 && cellular_response_gnss_active(resp) > 0) {
            rc = cellular_exchange_fd(fd, "AT#XNMEA=1", resp, sizeof(resp),
                                      NRF9151_CMD_TIMEOUT_US);
            cellular_update_from_response(CELLULAR_ACTION_GNSS_START,
                                          "AT#XNMEA=1", resp, rc);
            if(rc == 0) {
                cellular_try_led_mode(fd, 1, "search");
            }
            cellular_set_summary(cellular_gnss_status,
                                 sizeof(cellular_gnss_status),
                                 rc == 0 ? "OK" : "FAIL");
            cellular_log_append("GNSS already running in LTE+GNSS mode");
            return rc == 0 ? 0 : 1;
        }
    }

    rc = cellular_exchange_fd(fd, "AT+CFUN=31", resp, sizeof(resp),
                              NRF9151_CMD_TIMEOUT_US);
    cellular_update_from_response(CELLULAR_ACTION_GNSS_START,
                                  "AT+CFUN=31", resp, rc);
    if(rc != 0) {
        failures++;
    }

    rc = cellular_exchange_fd(fd, "AT%XSYSTEMMODE?", resp, sizeof(resp),
                              NRF9151_CMD_TIMEOUT_US);
    cellular_update_from_response(CELLULAR_ACTION_GNSS_STATUS,
                                  "AT%XSYSTEMMODE?", resp, rc);
    if(rc != 0) {
        failures++;
    }

    rc = cellular_exchange_fd(fd, "AT#XNMEA=1", resp, sizeof(resp),
                              NRF9151_CMD_TIMEOUT_US);
    cellular_update_from_response(CELLULAR_ACTION_GNSS_START,
                                  "AT#XNMEA=1", resp, rc);
    if(rc != 0) {
        failures++;
    }

    pthread_mutex_lock(&cellular_lock);
    cellular_gnss_session_start_locked();
    pthread_mutex_unlock(&cellular_lock);
    cellular_log_append("GNSS TTFF timer reset");
    cellular_try_led_mode(fd, 1, "search");

    rc = cellular_exchange_fd(fd, "AT#XGNSS=1,0,0,0", resp, sizeof(resp),
                              NRF9151_CMD_TIMEOUT_US);
    cellular_update_from_response(CELLULAR_ACTION_GNSS_START,
                                  "AT#XGNSS=1,0,0,0", resp, rc);
    if(rc != 0) {
        char status[1024];
        int status_rc = cellular_exchange_fd(fd, "AT#XGNSS?", status,
                                             sizeof(status),
                                             NRF9151_CMD_TIMEOUT_US);

        cellular_update_from_response(CELLULAR_ACTION_GNSS_STATUS,
                                      "AT#XGNSS?", status, status_rc);
        if(status_rc == 0 && cellular_response_gnss_active(status) > 0) {
            cellular_log_append("GNSS start returned ERROR but status is active");
            cellular_set_summary(cellular_gnss_status,
                                 sizeof(cellular_gnss_status), "OK");
            return failures;
        }
        failures++;
    }
    cellular_set_summary(cellular_gnss_status, sizeof(cellular_gnss_status),
                         failures == 0 ? "OK" : "FAIL");
    return failures;
}

static void cellular_run_full_test(int fd)
{
    int link_failures;
    int sim_failures;
    int lte_failures;
    int total_failures;

    pthread_mutex_lock(&cellular_lock);
    cellular_sim_positive_seen = 0;
    cellular_lte_sim_fault_seen = 0;
    snprintf(cellular_link_status, sizeof(cellular_link_status), "%s",
             "Testing");
    snprintf(cellular_sim_status, sizeof(cellular_sim_status), "%s",
             "Testing");
    snprintf(cellular_lte_status, sizeof(cellular_lte_status), "%s",
             "Testing");
    snprintf(cellular_imei_status, sizeof(cellular_imei_status), "%s", "--");
    snprintf(cellular_operator_status, sizeof(cellular_operator_status),
             "%s", "--");
    snprintf(cellular_ip_status, sizeof(cellular_ip_status), "%s", "--");
    cellular_network_rebuild_locked();
    pthread_mutex_unlock(&cellular_lock);

    cellular_log_append("LTE check: UART, SIM, registration, IP, ping");
    link_failures = cellular_run_command_list(fd, CELLULAR_ACTION_LINK,
                                              cellular_link_cmds,
                                              sizeof(cellular_link_cmds) /
                                              sizeof(cellular_link_cmds[0]));
    sim_failures = cellular_run_command_list(fd, CELLULAR_ACTION_SIM,
                                             cellular_sim_cmds,
                                             sizeof(cellular_sim_cmds) /
                                             sizeof(cellular_sim_cmds[0]));
    lte_failures = cellular_run_command_list(fd, CELLULAR_ACTION_LTE_STATUS,
                                             cellular_lte_status_cmds,
                                             sizeof(cellular_lte_status_cmds) /
                                             sizeof(cellular_lte_status_cmds[0]));
    total_failures = link_failures + sim_failures + lte_failures;
    cellular_set_status(total_failures == 0 ? "Check OK" : "Check issues");
    cellular_log_append("LTE check done failures=%d link=%d sim=%d lte=%d",
                        total_failures, link_failures, sim_failures,
                        lte_failures);
}

static int cellular_cno_monitor_should_stop(void)
{
    int stop;

    pthread_mutex_lock(&cellular_lock);
    stop = cellular_cno_monitor_stop;
    pthread_mutex_unlock(&cellular_lock);
    return stop;
}

static void *cellular_cno_monitor_main(void *arg)
{
    (void)arg;
    int fd;
    char uart_line[NRF9151_UART_LINE_MAX];
    size_t uart_used = 0;
    unsigned int idle_ticks = 0;
    unsigned int last_nmea_count = 0;
    int restart_attempted = 0;

    cellular_set_status("GNSS");
    cellular_log_append("=== GNSS monitor start ===");
#if NRF9151_EN_CONTROL_ENABLED
    if(cellular_gpio_set(NRF9151_EN_GPIO, 1) == 0) {
        cellular_log_append("GPIO%u EN=1", NRF9151_EN_GPIO);
        usleep(600000);
    } else {
        cellular_log_append("GPIO%u EN set failed: %s", NRF9151_EN_GPIO,
                            strerror(errno));
    }
#endif
    fd = cellular_open_uart();
    if(fd < 0) {
        cellular_log_append("open %s failed: %s", NRF9151_UART_DEV,
                            strerror(errno));
        cellular_set_status("UART open failed");
        goto out;
    }

    cellular_run_gnss_start_lte_mode(fd, 0);
    pthread_mutex_lock(&cellular_lock);
    last_nmea_count = cellular_nmea_count;
    pthread_mutex_unlock(&cellular_lock);
    cellular_log_append("GNSS monitor running; press GNSS again to stop");
    while(!cellular_cno_monitor_should_stop()) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        if(select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            idle_ticks++;
            if(idle_ticks == 40U) {
                unsigned int current_nmea_count;

                pthread_mutex_lock(&cellular_lock);
                current_nmea_count = cellular_nmea_count;
                pthread_mutex_unlock(&cellular_lock);
                if(current_nmea_count == last_nmea_count) {
                    if(!restart_attempted) {
                        cellular_log_append("GNSS monitor no NMEA; restart GNSS");
                        cellular_run_gnss_start(fd, 1);
                        pthread_mutex_lock(&cellular_lock);
                        last_nmea_count = cellular_nmea_count;
                        pthread_mutex_unlock(&cellular_lock);
                        restart_attempted = 1;
                    } else {
                        cellular_log_append("GNSS monitor waiting for NMEA");
                    }
                } else {
                    last_nmea_count = current_nmea_count;
                    restart_attempted = 0;
                }
                idle_ticks = 0;
            }
            continue;
        }
        rd = read(fd, buf, sizeof(buf) - 1);
        if(rd <= 0) {
            continue;
        }
        idle_ticks = 0;
        cellular_uart_line_feed(uart_line, &uart_used, buf, (size_t)rd);
        cellular_apply_led_auto_if_pending(fd);
        pthread_mutex_lock(&cellular_lock);
        last_nmea_count = cellular_nmea_count;
        pthread_mutex_unlock(&cellular_lock);
    }
    if(uart_used > 0U) {
        uart_line[uart_used] = '\0';
        cellular_process_uart_line(uart_line);
        cellular_apply_led_auto_if_pending(fd);
    }
    cellular_log_append("=== GNSS monitor stop ===");
    cellular_set_status("GNSS stopped");
    cellular_close_uart(fd);

out:
    pthread_mutex_lock(&cellular_lock);
    cellular_cno_monitor_active = 0;
    cellular_cno_monitor_stop = 0;
    pthread_mutex_unlock(&cellular_lock);
    return NULL;
}

static void cellular_start_cno_monitor_after_check(void)
{
    pthread_t thread;
    int start = 0;

    pthread_mutex_lock(&cellular_lock);
    if(!cellular_cno_monitor_active) {
        if(!cellular_page_active) {
            pthread_mutex_unlock(&cellular_lock);
            return;
        }
        cellular_cno_monitor_active = 1;
        cellular_cno_monitor_stop = 0;
        start = 1;
    }
    pthread_mutex_unlock(&cellular_lock);

    if(!start) {
        return;
    }

    if(pthread_create(&thread, NULL, cellular_cno_monitor_main, NULL) == 0) {
        pthread_detach(thread);
        cellular_log_append("GNSS NMEA parser monitor started");
    } else {
        pthread_mutex_lock(&cellular_lock);
        cellular_cno_monitor_active = 0;
        cellular_cno_monitor_stop = 0;
        pthread_mutex_unlock(&cellular_lock);
        cellular_log_append("pthread_create GNSS monitor failed");
    }
    app_request_fast_refresh();
}

static void cellular_action_commands(cellular_action_t action,
                                     const char *const **cmds, size_t *count,
                                     const char **title)
{
    *cmds = NULL;
    *count = 0;
    *title = "nRF9151 test";

    switch(action) {
    case CELLULAR_ACTION_LINK:
        *cmds = cellular_link_cmds;
        *count = sizeof(cellular_link_cmds) / sizeof(cellular_link_cmds[0]);
        *title = "UART link test";
        break;
    case CELLULAR_ACTION_SIM:
        *cmds = cellular_sim_cmds;
        *count = sizeof(cellular_sim_cmds) / sizeof(cellular_sim_cmds[0]);
        *title = "SIM card";
        break;
    case CELLULAR_ACTION_LTE_STATUS:
        *cmds = cellular_lte_status_cmds;
        *count = sizeof(cellular_lte_status_cmds) /
                 sizeof(cellular_lte_status_cmds[0]);
        *title = "LTE status";
        break;
    case CELLULAR_ACTION_LTE_GNSS_MODE:
        *cmds = cellular_lte_gnss_mode_cmds;
        *count = sizeof(cellular_lte_gnss_mode_cmds) /
                 sizeof(cellular_lte_gnss_mode_cmds[0]);
        *title = "LTE+GNSS mode";
        break;
    case CELLULAR_ACTION_GNSS_START:
        *cmds = cellular_gnss_start_cmds;
        *count = sizeof(cellular_gnss_start_cmds) /
                 sizeof(cellular_gnss_start_cmds[0]);
        *title = "Start GNSS";
        break;
    case CELLULAR_ACTION_GNSS_STATUS:
        *cmds = cellular_gnss_status_cmds;
        *count = sizeof(cellular_gnss_status_cmds) /
                 sizeof(cellular_gnss_status_cmds[0]);
        *title = "GNSS status";
        break;
    case CELLULAR_ACTION_GNSS_NMEA:
        *title = "Read NMEA";
        break;
    case CELLULAR_ACTION_GNSS_STOP:
        *cmds = cellular_gnss_stop_cmds;
        *count = sizeof(cellular_gnss_stop_cmds) /
                 sizeof(cellular_gnss_stop_cmds[0]);
        *title = "Stop GNSS";
        break;
    case CELLULAR_ACTION_FULL_TEST:
        *title = "LTE";
        break;
    case CELLULAR_ACTION_HTTP_GET:
        *title = "HTTP GET";
        break;
    case CELLULAR_ACTION_HTTP_POST:
        *title = "HTTP POST";
        break;
    case CELLULAR_ACTION_MQTT_TEST:
        *title = "MQTT";
        break;
    }
}

static int cellular_action_uses_lte_manager(cellular_action_t action)
{
    return action == CELLULAR_ACTION_LINK ||
           action == CELLULAR_ACTION_SIM ||
           action == CELLULAR_ACTION_LTE_STATUS ||
           action == CELLULAR_ACTION_LTE_GNSS_MODE ||
           action == CELLULAR_ACTION_FULL_TEST ||
           action == CELLULAR_ACTION_HTTP_GET ||
           action == CELLULAR_ACTION_HTTP_POST ||
           action == CELLULAR_ACTION_MQTT_TEST;
}

static int cellular_generation_active(unsigned int generation)
{
    int active;

    pthread_mutex_lock(&cellular_lock);
    active = cellular_page_active && generation == cellular_action_generation;
    pthread_mutex_unlock(&cellular_lock);
    return active;
}

static int cellular_action_cancel_cb(void *user_data)
{
    unsigned int generation = (unsigned int)(uintptr_t)user_data;

    return !cellular_generation_active(generation);
}

static void cellular_copy_field(const char *text, int index,
                                char *out, size_t out_len)
{
    const char *start = text ? text : "";
    const char *end;
    int current = 0;
    size_t len;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    while(current < index && *start) {
        if(*start++ == '|') {
            current++;
        }
    }
    end = start;
    while(*end && *end != '|') {
        end++;
    }
    len = (size_t)(end - start);
    if(len >= out_len) {
        len = out_len - 1U;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    ui_trim_text(out);
}

static int cellular_run_manager_action(cellular_action_t action,
                                       unsigned int generation)
{
    k230_nrf9151_status_t status;
    char log[NRF9151_LOG_MAX];
    int rc = -1;

    log[0] = '\0';
    if(cellular_action_uses_lte_manager(action)) {
        int stop_rc = k230_nrf9151_stop_gnss_monitor_wait(6000);

        pthread_mutex_lock(&cellular_lock);
        cellular_cno_monitor_active = 0;
        cellular_cno_monitor_stop = 1;
        cellular_manager_gnss_started = 0;
        pthread_mutex_unlock(&cellular_lock);
        if(stop_rc != 0 && k230_nrf9151_gnss_monitor_active()) {
            snprintf(log, sizeof(log), "%s",
                     "GNSS monitor still owns UART");
            cellular_log_append("%s", log);
            cellular_set_status("nRF9151 busy");
            return -1;
        }
    }

    if(action == CELLULAR_ACTION_LINK ||
       action == CELLULAR_ACTION_SIM ||
       action == CELLULAR_ACTION_LTE_STATUS ||
       action == CELLULAR_ACTION_LTE_GNSS_MODE ||
       action == CELLULAR_ACTION_FULL_TEST) {
        rc = k230_nrf9151_run_lte_check_ex(
            &status, log, sizeof(log), cellular_action_cancel_cb,
            (void *)(uintptr_t)generation);
        cellular_apply_manager_status(&status);
    } else if(action == CELLULAR_ACTION_HTTP_GET ||
              action == CELLULAR_ACTION_HTTP_POST) {
        k230_nrf9151_http_request_t req;
        char url[256];
        char body[256];

        memset(&req, 0, sizeof(req));
        if(action == CELLULAR_ACTION_HTTP_GET) {
            snprintf(url, sizeof(url), "%s", cellular_http_url);
            body[0] = '\0';
            req.method = K230_NRF9151_HTTP_GET;
        } else {
            cellular_copy_field(cellular_http_post_spec, 0, url, sizeof(url));
            cellular_copy_field(cellular_http_post_spec, 1, body, sizeof(body));
            req.method = K230_NRF9151_HTTP_POST;
            req.body = body;
        }
        req.url = url;
        req.sec_tag = -1;
        rc = k230_nrf9151_http_request_ex(
            &req, log, sizeof(log), cellular_action_cancel_cb,
            (void *)(uintptr_t)generation);
    } else if(action == CELLULAR_ACTION_MQTT_TEST) {
        k230_nrf9151_mqtt_request_t req;
        char broker[160];
        char port_text[16];
        char user[96];
        char pass[96];
        char topic[160];
        char payload[256];
        char sec_tag_text[16];

        memset(&req, 0, sizeof(req));
        cellular_copy_field(cellular_mqtt_spec, 0, broker, sizeof(broker));
        cellular_copy_field(cellular_mqtt_spec, 1, port_text,
                            sizeof(port_text));
        cellular_copy_field(cellular_mqtt_spec, 2, user, sizeof(user));
        cellular_copy_field(cellular_mqtt_spec, 3, pass, sizeof(pass));
        cellular_copy_field(cellular_mqtt_spec, 4, topic, sizeof(topic));
        cellular_copy_field(cellular_mqtt_spec, 5, payload, sizeof(payload));
        cellular_copy_field(cellular_mqtt_spec, 6, sec_tag_text,
                            sizeof(sec_tag_text));
        req.broker = broker;
        req.port = port_text[0] ? atoi(port_text) : 1883;
        req.client_id = "k230-nrf9151";
        req.username = user;
        req.password = pass;
        req.topic = topic[0] ? topic : "k230/test";
        req.payload = payload;
        req.qos = 0;
        req.retain = 0;
        req.sec_tag = sec_tag_text[0] ? atoi(sec_tag_text) : -1;
        req.auth = user[0] || pass[0] ?
                   K230_NRF9151_MQTT_AUTH_USER_PASS :
                   K230_NRF9151_MQTT_AUTH_NONE;
        rc = k230_nrf9151_mqtt_test_ex(
            &req, log, sizeof(log), cellular_action_cancel_cb,
            (void *)(uintptr_t)generation);
    } else if(action == CELLULAR_ACTION_GNSS_START ||
              action == CELLULAR_ACTION_GNSS_NMEA ||
              action == CELLULAR_ACTION_GNSS_STATUS) {
        rc = k230_nrf9151_start_gnss_monitor();
        pthread_mutex_lock(&cellular_lock);
        cellular_cno_monitor_active = (rc == 0);
        cellular_manager_gnss_started = (rc == 0);
        pthread_mutex_unlock(&cellular_lock);
        snprintf(log, sizeof(log), "%s", rc == 0 ?
                 "GNSS manager monitor started" :
                 "GNSS manager monitor start failed");
        if(k230_nrf9151_read_status(&status, 0) == 0) {
            cellular_apply_manager_status(&status);
        }
    } else if(action == CELLULAR_ACTION_GNSS_STOP) {
        rc = k230_nrf9151_stop_gnss_monitor();
        pthread_mutex_lock(&cellular_lock);
        cellular_cno_monitor_active = 0;
        cellular_manager_gnss_started = 0;
        pthread_mutex_unlock(&cellular_lock);
        snprintf(log, sizeof(log), "%s", "GNSS manager monitor stopped");
    }

    if(log[0]) {
        char *saveptr = NULL;
        char *line = strtok_r(log, "\n", &saveptr);

        while(line) {
            cellular_log_append("%s", line);
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    if(cellular_generation_active(generation)) {
        cellular_set_status(rc == -2 ? "%s canceled" :
                            (rc == 0 ? "%s OK" : "%s issues"),
                            action == CELLULAR_ACTION_HTTP_GET ? "HTTP GET" :
                            action == CELLULAR_ACTION_HTTP_POST ? "HTTP POST" :
                            action == CELLULAR_ACTION_MQTT_TEST ? "MQTT" :
                            cellular_action_uses_lte_manager(action) ? "LTE" :
                            "GNSS");
    }
    return rc;
}

static void *cellular_worker_main(void *arg)
{
    cellular_worker_ctx_t *ctx = (cellular_worker_ctx_t *)arg;
    cellular_action_t action;
    unsigned int generation;
    const char *const *cmds;
    const char *title;
    size_t count;
    int rc;

    if(!ctx) {
        return NULL;
    }
    action = ctx->action;
    generation = ctx->generation;
    free(ctx);

    cellular_action_commands(action, &cmds, &count, &title);
    (void)cmds;
    (void)count;
    cellular_set_status("%s", title);
    cellular_log_append("=== %s ===", title);
    if(action == CELLULAR_ACTION_SIM || action == CELLULAR_ACTION_FULL_TEST) {
        pthread_mutex_lock(&cellular_lock);
        cellular_sim_positive_seen = 0;
        snprintf(cellular_sim_status, sizeof(cellular_sim_status),
                 "%s", "Testing");
        pthread_mutex_unlock(&cellular_lock);
    }
    if(action == CELLULAR_ACTION_LTE_STATUS ||
       action == CELLULAR_ACTION_LTE_GNSS_MODE) {
        pthread_mutex_lock(&cellular_lock);
        cellular_lte_sim_fault_seen = 0;
        snprintf(cellular_lte_status, sizeof(cellular_lte_status),
                 "%s", "Testing");
        pthread_mutex_unlock(&cellular_lock);
    }

    rc = cellular_run_manager_action(action, generation);
    if(cellular_generation_active(generation)) {
        cellular_set_action_result(action, rc == 0);
    }

    pthread_mutex_lock(&cellular_lock);
    if(generation == cellular_action_generation) {
        cellular_worker_active = 0;
    }
    pthread_mutex_unlock(&cellular_lock);
    return NULL;
}

static void cellular_start_action(cellular_action_t action)
{
    pthread_t thread;
    cellular_worker_ctx_t *ctx;
    const char *const *cmds;
    const char *title;
    size_t count;
    int busy = 0;
    unsigned int generation = 0;

    cellular_action_commands(action, &cmds, &count, &title);
    (void)cmds;
    (void)count;
    ctx = calloc(1, sizeof(*ctx));
    if(!ctx) {
        cellular_set_status("thread failed");
        cellular_log_append("calloc worker context failed");
        return;
    }
    pthread_mutex_lock(&cellular_lock);
    busy = cellular_worker_active ||
           (cellular_cno_monitor_active &&
            !cellular_action_uses_lte_manager(action) &&
            action != CELLULAR_ACTION_GNSS_STOP);
    if(!busy) {
        cellular_worker_active = 1;
        generation = ++cellular_action_generation;
        snprintf(cellular_running_title, sizeof(cellular_running_title), "%s",
                 title);
    }
    pthread_mutex_unlock(&cellular_lock);

    if(busy) {
        free(ctx);
        cellular_log_append("Action ignored: stop GNSS monitor or wait for worker");
        return;
    }

    ctx->action = action;
    ctx->generation = generation;
    if(pthread_create(&thread, NULL, cellular_worker_main, ctx) == 0) {
        pthread_detach(thread);
    } else {
        free(ctx);
        pthread_mutex_lock(&cellular_lock);
        if(generation == cellular_action_generation) {
            cellular_worker_active = 0;
        }
        pthread_mutex_unlock(&cellular_lock);
        cellular_set_status("thread failed");
        cellular_log_append("pthread_create failed");
    }
    app_request_fast_refresh();
}

static void cellular_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    cellular_status_refresh();
    cellular_log_refresh();
}

static void cellular_action_event_cb(lv_event_t *event)
{
    cellular_action_t action =
        (cellular_action_t)(intptr_t)lv_event_get_user_data(event);

    cellular_start_action(action);
}

static void cellular_http_get_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    snprintf(cellular_http_url, sizeof(cellular_http_url), "%s",
             text && text[0] ? text : "http://example.com/");
    ui_trim_text(cellular_http_url);
    cellular_start_action(CELLULAR_ACTION_HTTP_GET);
}

static void cellular_http_post_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    snprintf(cellular_http_post_spec, sizeof(cellular_http_post_spec), "%s",
             text && text[0] ? text : "http://example.com/|hello=k230");
    ui_trim_text(cellular_http_post_spec);
    cellular_start_action(CELLULAR_ACTION_HTTP_POST);
}

static void cellular_mqtt_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    snprintf(cellular_mqtt_spec, sizeof(cellular_mqtt_spec), "%s",
             text && text[0] ? text :
             "broker.hivemq.com|1883|||k230/test|hello from k230|-1");
    ui_trim_text(cellular_mqtt_spec);
    cellular_start_action(CELLULAR_ACTION_MQTT_TEST);
}

static void cellular_http_get_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    (void)event;
    memset(&config, 0, sizeof(config));
    config.title = "HTTP GET";
    config.placeholder = "http://host/path";
    config.initial_text = cellular_http_url;
    config.max_length = sizeof(cellular_http_url) - 1U;
    config.min_length = 8U;
    config.min_length_text = "Enter http:// or https:// URL";
    config.submit_cb = cellular_http_get_submit_cb;
    config.submit_text = "Run";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void cellular_http_post_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    (void)event;
    memset(&config, 0, sizeof(config));
    config.title = "HTTP POST";
    config.placeholder = "http://host/path|body";
    config.initial_text = cellular_http_post_spec;
    config.max_length = sizeof(cellular_http_post_spec) - 1U;
    config.min_length = 8U;
    config.min_length_text = "Use URL|body";
    config.submit_cb = cellular_http_post_submit_cb;
    config.submit_text = "Run";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void cellular_mqtt_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    (void)event;
    memset(&config, 0, sizeof(config));
    config.title = "MQTT";
    config.placeholder = "broker|port|user|pass|topic|payload|sec_tag";
    config.initial_text = cellular_mqtt_spec;
    config.max_length = sizeof(cellular_mqtt_spec) - 1U;
    config.min_length = 3U;
    config.min_length_text = "Enter MQTT broker";
    config.submit_cb = cellular_mqtt_submit_cb;
    config.submit_text = "Run";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void cellular_cno_monitor_event_cb(lv_event_t *event)
{
    int active;
    int busy;
    int rc = 0;

    (void)event;
    pthread_mutex_lock(&cellular_lock);
    active = cellular_cno_monitor_active ||
             k230_nrf9151_gnss_monitor_active();
    busy = cellular_worker_active;
    pthread_mutex_unlock(&cellular_lock);

    if(active) {
        rc = k230_nrf9151_stop_gnss_monitor();
        pthread_mutex_lock(&cellular_lock);
        cellular_cno_monitor_active = 0;
        cellular_cno_monitor_stop = 0;
        cellular_manager_gnss_started = 0;
        pthread_mutex_unlock(&cellular_lock);
        cellular_log_append("GNSS manager monitor stop requested");
        cellular_set_status(rc == 0 ? "GNSS stopped" : "GNSS stop failed");
        app_request_fast_refresh();
        return;
    }
    if(busy) {
        cellular_log_append("GNSS monitor ignored: worker busy");
        app_request_fast_refresh();
        return;
    }

    rc = k230_nrf9151_start_gnss_monitor();
    pthread_mutex_lock(&cellular_lock);
    cellular_cno_monitor_active = (rc == 0);
    cellular_cno_monitor_stop = 0;
    cellular_manager_gnss_started = (rc == 0);
    pthread_mutex_unlock(&cellular_lock);
    if(rc == 0) {
        cellular_log_append("GNSS manager monitor started");
        cellular_set_status("GNSS");
    } else {
        cellular_log_append("GNSS manager monitor start failed");
        cellular_set_status("GNSS failed");
    }
    app_request_fast_refresh();
}

static void cellular_clear_event_cb(lv_event_t *event)
{
    (void)event;
    pthread_mutex_lock(&cellular_lock);
    cellular_log[0] = '\0';
    snprintf(cellular_status, sizeof(cellular_status), "%s", "Ready");
    cellular_gnss_reset_locked();
    pthread_mutex_unlock(&cellular_lock);
    unlink(NRF9151_TEST_LOG);
    cellular_log_refresh();
    cellular_status_refresh();
}

static lv_obj_t *cellular_status_card(lv_obj_t *parent, int x, int y, int w,
                                      const char *title, uint32_t color,
                                      lv_obj_t **value_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, 76);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    label = ui_label(card, title, &lv_font_montserrat_16, color);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    *value_label = ui_label(card, "Not tested", &lv_font_montserrat_14,
                            0xDCE5EE);
    lv_obj_set_width(*value_label, w - 20);
    lv_label_set_long_mode(*value_label, LV_LABEL_LONG_DOT);
    lv_obj_align(*value_label, LV_ALIGN_TOP_LEFT, 0, 30);
    return card;
}

static lv_obj_t *cellular_button(lv_obj_t *parent, int x, int y, int w,
                                 const char *text, uint32_t color,
                                 lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = ui_command_button(parent, x, y, w, text, color);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

void ui_cellular_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *summary;
    lv_obj_t *info_panel;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(144);
    int margin = ui_page_side_margin();
    int panel_w = ui_page_panel_width();
    int gap = landscape ? 20 : 24;
    int left_x = margin;
    int left_w = landscape ? 430 : panel_w;
    int right_x = landscape ? left_x + left_w + gap : left_x;
    int right_w = landscape ? panel_w - left_w - gap : panel_w;
    int summary_y = landscape ? 16 : 20;
    int summary_h = landscape ? body_h - 32 : 430;
    int info_y = landscape ? 16 : summary_y + summary_h + 24;
    int info_h = landscape ? body_h - 32 : body_h - info_y - 24;
    int card_w = (left_w - 44) / 2;
    int action_btn_w = (left_w - 56) / 3;
    int info_inner_w = right_w - 48;
    int status_y0 = landscape ? 82 : 88;
    int status_row_gap = 8;
    int button_y = summary_h - 82;
    int gps_label_h = landscape ? 58 : 76;
    int ttff_y = 42 + gps_label_h + 8;
    int chart_y = ttff_y + 34;
    int net_button_y = chart_y + 162;
    int net_btn_w = (info_inner_w - 24) / 3;
    int log_title_y = net_button_y + 52;
    int log_text_y = log_title_y + 38;

    pthread_mutex_lock(&cellular_lock);
    cellular_page_active = 1;
    pthread_mutex_unlock(&cellular_lock);

    if(info_h < 360) {
        info_h = 360;
    }

    ui_create_header(scr, "Cellular");
    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    summary = ui_panel(body, left_x, summary_y, left_w, summary_h);
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x151B22), 0);

    ui_label(summary, "nRF9151 modem", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(summary, lv_obj_get_child_count(summary) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    cellular_status_label = ui_label(summary, "--", &lv_font_montserrat_16,
                                     0x9AA4AF);
    lv_obj_set_width(cellular_status_label, left_w - 32);
    lv_label_set_long_mode(cellular_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(cellular_status_label, LV_ALIGN_TOP_LEFT, 0, 46);

    cellular_status_card(summary, 0, status_y0, card_w, "Link", 0x60A5FA,
                         &cellular_link_label);
    cellular_status_card(summary, card_w + 12, status_y0, card_w,
                         "SIM card", 0xA3E635, &cellular_sim_label);
    cellular_status_card(summary, 0, status_y0 + 76 + status_row_gap,
                         card_w, "LTE", 0x25C281,
                         &cellular_lte_label);
    cellular_status_card(summary, card_w + 12,
                         status_y0 + 76 + status_row_gap,
                         card_w, "GNSS", 0xF5A524, &cellular_gnss_label);
    cellular_status_card(summary, 0,
                         status_y0 + (76 + status_row_gap) * 2,
                         card_w, "IMEI", 0x38BDF8,
                         &cellular_imei_label);
    cellular_status_card(summary, card_w + 12,
                         status_y0 + (76 + status_row_gap) * 2,
                         card_w, "Network", 0x34D399,
                         &cellular_network_label);

    cellular_button(summary, 0, button_y, action_btn_w,
                    "LTE", 0x25C281,
                    cellular_action_event_cb,
                    (void *)(intptr_t)CELLULAR_ACTION_FULL_TEST);
    cellular_cno_button = cellular_button(summary, action_btn_w + 12,
                                          button_y, action_btn_w,
                                          "GNSS", 0xF97316,
                                          cellular_cno_monitor_event_cb, NULL);
    cellular_button(summary,
                    (action_btn_w + 12) * 2, button_y,
                    action_btn_w, "Clear log",
                    0x94A3B8,
                    cellular_clear_event_cb, NULL);

    info_panel = ui_panel(body, right_x, info_y, right_w, info_h);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0x101820), 0);
    lv_obj_add_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(info_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(info_panel, 36, 0);

    ui_label(info_panel, "GPS information", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(info_panel,
                                  lv_obj_get_child_count(info_panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    cellular_gps_label = ui_label(info_panel, "Waiting for NMEA",
                                  &lv_font_montserrat_16, 0xDCE5EE);
    lv_obj_set_width(cellular_gps_label, info_inner_w);
    lv_obj_set_height(cellular_gps_label, gps_label_h);
    lv_label_set_long_mode(cellular_gps_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(cellular_gps_label, LV_ALIGN_TOP_LEFT, 0, 42);

    cellular_ttff_label = ui_label(info_panel, "TTFF: not started",
                                   &lv_font_montserrat_16, 0xF5A524);
    lv_obj_set_width(cellular_ttff_label, info_inner_w);
    lv_label_set_long_mode(cellular_ttff_label, LV_LABEL_LONG_DOT);
    lv_obj_align(cellular_ttff_label, LV_ALIGN_TOP_LEFT, 0, ttff_y);

    cellular_sat_chart = cellular_cn0_chart_create(info_panel, info_inner_w);
    lv_obj_align(cellular_sat_chart, LV_ALIGN_TOP_LEFT, 0, chart_y);
    cellular_sat_label = NULL;

    cellular_button(info_panel, 0, net_button_y, net_btn_w,
                    "HTTP GET", 0x38BDF8,
                    cellular_http_get_event_cb, NULL);
    cellular_button(info_panel, net_btn_w + 12, net_button_y, net_btn_w,
                    "HTTP POST", 0x60A5FA,
                    cellular_http_post_event_cb, NULL);
    cellular_button(info_panel, (net_btn_w + 12) * 2, net_button_y, net_btn_w,
                    "MQTT", 0xA78BFA,
                    cellular_mqtt_event_cb, NULL);

    ui_label(info_panel, "Serial log", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(info_panel,
                                  lv_obj_get_child_count(info_panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_y(lv_obj_get_child(info_panel,
                                  lv_obj_get_child_count(info_panel) - 1),
                 log_title_y);

    cellular_log_label = ui_label(info_panel, "No log yet",
                                  &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(cellular_log_label, info_inner_w);
    lv_label_set_long_mode(cellular_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(cellular_log_label, LV_ALIGN_TOP_LEFT, 0, log_text_y);

    cellular_check_panel = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(cellular_check_panel);
    lv_obj_set_style_bg_color(cellular_check_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cellular_check_panel, LV_OPA_60, 0);
    lv_obj_set_style_border_width(cellular_check_panel, 0, 0);
    lv_obj_set_style_pad_all(cellular_check_panel, 0, 0);
    lv_obj_add_flag(cellular_check_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cellular_check_panel, LV_OBJ_FLAG_SCROLLABLE);

    cellular_check_dialog = lv_obj_create(cellular_check_panel);
    lv_obj_set_size(cellular_check_dialog, landscape ? 360 : 320, 174);
    lv_obj_set_style_bg_color(cellular_check_dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(cellular_check_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cellular_check_dialog, 14, 0);
    lv_obj_set_style_border_width(cellular_check_dialog, 1, 0);
    lv_obj_set_style_border_color(cellular_check_dialog, lv_color_hex(0x25303A),
                                  0);
    lv_obj_set_style_pad_all(cellular_check_dialog, 0, 0);
    lv_obj_clear_flag(cellular_check_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(cellular_check_dialog);

    cellular_check_spinner = lv_spinner_create(cellular_check_dialog);
    lv_obj_set_size(cellular_check_spinner, 72, 72);
    lv_obj_align(cellular_check_spinner, LV_ALIGN_TOP_MID, 0, 22);
    cellular_check_label = ui_label(cellular_check_dialog, "Checking LTE",
                                    &lv_font_montserrat_18, 0xDCE5EE);
    lv_obj_set_width(cellular_check_label, landscape ? 260 : 240);
    lv_label_set_long_mode(cellular_check_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(cellular_check_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(cellular_check_label, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_add_flag(cellular_check_panel, LV_OBJ_FLAG_HIDDEN);

    cellular_status_refresh();
    cellular_log_refresh();
    cellular_timer = lv_timer_create(cellular_timer_cb, 500, NULL);
}

void ui_cellular_cleanup(void)
{
    int stop_manager = 0;

    pthread_mutex_lock(&cellular_lock);
    cellular_page_active = 0;
    cellular_action_generation++;
    cellular_worker_active = 0;
    if(cellular_cno_monitor_active) {
        cellular_cno_monitor_stop = 1;
    }
    stop_manager = cellular_manager_gnss_started;
    cellular_manager_gnss_started = 0;
    cellular_cno_monitor_active = 0;
    snprintf(cellular_running_title, sizeof(cellular_running_title), "%s",
             "LTE");
    pthread_mutex_unlock(&cellular_lock);
    if(stop_manager) {
        (void)k230_nrf9151_stop_gnss_monitor();
        cellular_log_append("Cellular page cleanup: GNSS manager stop requested");
    }

    if(cellular_timer) {
        lv_timer_delete(cellular_timer);
        cellular_timer = NULL;
    }
    cellular_status_label = NULL;
    cellular_link_label = NULL;
    cellular_sim_label = NULL;
    cellular_lte_label = NULL;
    cellular_gnss_label = NULL;
    cellular_imei_label = NULL;
    cellular_network_label = NULL;
    cellular_gps_label = NULL;
    cellular_ttff_label = NULL;
    cellular_sat_chart = NULL;
    cellular_sat_label = NULL;
    cellular_log_label = NULL;
    cellular_cno_button = NULL;
    if(cellular_check_panel) {
        lv_obj_delete(cellular_check_panel);
    }
    cellular_check_panel = NULL;
    cellular_check_dialog = NULL;
    cellular_check_spinner = NULL;
    cellular_check_label = NULL;
    cellular_check_was_active = 0;
    cellular_check_hide_us = 0;
    memset(cellular_cn0_bars, 0, sizeof(cellular_cn0_bars));
}
