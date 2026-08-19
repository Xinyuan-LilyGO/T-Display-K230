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
int k230_nrf9151_uart_present(void);
int k230_nrf9151_read_gnss_fix(k230_nrf9151_gnss_fix_t *fix,
                               int max_age_seconds);
int k230_nrf9151_acquire_uart(const char *owner, int wait_ms);
void k230_nrf9151_release_uart(int lock_fd);

#ifdef __cplusplus
}
#endif

#endif
