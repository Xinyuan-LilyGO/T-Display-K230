#ifndef K230_PHONE_UI_NRF9151_MANAGER_H
#define K230_PHONE_UI_NRF9151_MANAGER_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define K230_NRF9151_UART_DEV "/dev/ttyS3"
#define K230_NRF9151_UART_LOCK "/tmp/k230_nrf9151_uart.lock"
#define K230_NRF9151_GNSS_FIX_CACHE "/tmp/k230_nrf9151_gnss_fix.cache"
#define K230_NRF9151_GNSS_FIX_CACHE_TMP "/tmp/k230_nrf9151_gnss_fix.cache.tmp"
#define K230_NRF9151_STATUS_CACHE "/tmp/k230_nrf9151_status.cache"
#define K230_NRF9151_STATUS_CACHE_TMP "/tmp/k230_nrf9151_status.cache.tmp"

typedef enum {
    K230_NRF9151_HTTP_GET = 0,
    K230_NRF9151_HTTP_POST = 1,
} k230_nrf9151_http_method_t;

typedef struct {
    int present;
    int link_ok;
    int sim_ready;
    int lte_registered;
    int packet_attached;
    int pdp_active;
    int gnss_running;
    int gnss_has_fix;
    int lte_signal_level;
    unsigned int nmea_rx_count;
    unsigned int nmea_valid_count;
    unsigned int nmea_nofix_count;
    unsigned int satellites;
    unsigned long ttff_ms;
    unsigned long last_nmea_ms;
    double latitude;
    double longitude;
    double altitude_m;
    int has_altitude;
    time_t epoch;
    long age_seconds;
    char modem_state[32];
    char gps_state[32];
    char gnss_phase[32];
    char imei[64];
    char firmware[64];
    char sim_status[48];
    char operator_name[96];
    char ip[96];
    char lte_status[160];
    char gnss_status[160];
    char last_error[160];
} k230_nrf9151_status_t;

typedef struct {
    const char *url;
    k230_nrf9151_http_method_t method;
    const char *body;
    char *response_body;
    size_t response_body_len;
    int *status_code;
    int sec_tag;
} k230_nrf9151_http_request_t;

typedef enum {
    K230_NRF9151_MQTT_AUTH_NONE = 0,
    K230_NRF9151_MQTT_AUTH_USER_PASS = 1,
    K230_NRF9151_MQTT_AUTH_TLS_USER_PASS = 2,
    K230_NRF9151_MQTT_AUTH_MTLS = 3,
} k230_nrf9151_mqtt_auth_t;

typedef struct {
    const char *broker;
    int port;
    const char *client_id;
    const char *username;
    const char *password;
    const char *topic;
    const char *payload;
    int qos;
    int retain;
    int sec_tag;
    k230_nrf9151_mqtt_auth_t auth;
} k230_nrf9151_mqtt_request_t;

typedef int (*k230_nrf9151_cancel_cb_t)(void *user_data);

typedef struct {
    int valid;
    double latitude;
    double longitude;
    double altitude_m;
    int has_altitude;
    int satellites;
    time_t epoch;
    long age_seconds;
    char source[48];
} k230_nrf9151_gnss_fix_t;

const char *k230_nrf9151_uart_dev(void);
const char *k230_nrf9151_gnss_cache_path(void);
const char *k230_nrf9151_gnss_cache_tmp_path(void);
const char *k230_nrf9151_status_cache_path(void);
int k230_nrf9151_uart_present(void);
void k230_nrf9151_status_init(k230_nrf9151_status_t *status);
int k230_nrf9151_read_status(k230_nrf9151_status_t *status,
                             int max_age_seconds);
int k230_nrf9151_write_status(const k230_nrf9151_status_t *status);
int k230_nrf9151_read_gnss_fix(k230_nrf9151_gnss_fix_t *fix,
                               int max_age_seconds);
int k230_nrf9151_write_gnss_fix(double latitude, double longitude,
                                int has_altitude, double altitude_m,
                                int satellites, const char *source);
int k230_nrf9151_acquire_uart(const char *owner, int wait_ms);
void k230_nrf9151_release_uart(int lock_fd);
int k230_nrf9151_start_gnss_monitor(void);
int k230_nrf9151_stop_gnss_monitor(void);
int k230_nrf9151_stop_gnss_monitor_wait(int wait_ms);
int k230_nrf9151_gnss_monitor_active(void);
int k230_nrf9151_run_lte_check(k230_nrf9151_status_t *status,
                               char *log, size_t log_len);
int k230_nrf9151_run_lte_check_ex(k230_nrf9151_status_t *status,
                                  char *log, size_t log_len,
                                  k230_nrf9151_cancel_cb_t cancel_cb,
                                  void *cancel_user);
int k230_nrf9151_http_request(const k230_nrf9151_http_request_t *request,
                              char *log, size_t log_len);
int k230_nrf9151_http_request_ex(const k230_nrf9151_http_request_t *request,
                                 char *log, size_t log_len,
                                 k230_nrf9151_cancel_cb_t cancel_cb,
                                 void *cancel_user);
int k230_nrf9151_mqtt_test(const k230_nrf9151_mqtt_request_t *request,
                           char *log, size_t log_len);
int k230_nrf9151_mqtt_test_ex(const k230_nrf9151_mqtt_request_t *request,
                              char *log, size_t log_len,
                              k230_nrf9151_cancel_cb_t cancel_cb,
                              void *cancel_user);
int k230_nrf9151_mqtt_session_connect_ex(
    const k230_nrf9151_mqtt_request_t *request,
    char *log, size_t log_len,
    k230_nrf9151_cancel_cb_t cancel_cb,
    void *cancel_user);
int k230_nrf9151_mqtt_session_disconnect(char *log, size_t log_len);
int k230_nrf9151_mqtt_session_subscribe(const char *topic, int qos,
                                         char *log, size_t log_len,
                                         k230_nrf9151_cancel_cb_t cancel_cb,
                                         void *cancel_user);
int k230_nrf9151_mqtt_session_publish(const char *topic, const char *payload,
                                       int qos, int retain,
                                       char *log, size_t log_len,
                                       k230_nrf9151_cancel_cb_t cancel_cb,
                                       void *cancel_user);
int k230_nrf9151_mqtt_session_poll(char *log, size_t log_len,
                                    unsigned int timeout_ms,
                                    k230_nrf9151_cancel_cb_t cancel_cb,
                                    void *cancel_user);
int k230_nrf9151_mqtt_session_connected(void);

#ifdef __cplusplus
}
#endif

#endif
