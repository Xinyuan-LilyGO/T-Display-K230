#include "ui_nrf52840_manager.h"

#include "ui_common.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define NRF52840_MANAGER_LOG "/tmp/k230_nrf52840_manager.log"
#define NRF52840_DFU_TOOL "/root/app/k230_phone_ui/k230_nrf52840_dfu"
#define NRF52840_UART_DEV "/dev/ttyS1"
#define NRF52840_MESHTASTIC_PROC "k230_meshtastic_probe"
#define NRF52840_PROBE_RETRY_US 2000000ULL
#define NRF52840_STARTUP_PROBE_ATTEMPTS 4

static pthread_mutex_t nrf52840_lock = PTHREAD_MUTEX_INITIALIZER;
static nrf52840_status_t nrf52840_status;
static int nrf52840_started;
static int nrf52840_shutdown_requested;
static int nrf52840_probe_running;
static uint64_t nrf52840_next_probe_us;
static nrf52840_ble_mode_t nrf52840_dfu_previous_mode = NRF52840_MODE_MESHTASTIC;
static int nrf52840_dfu_previous_mode_valid;

static int nrf52840_run_at_query(char *response, size_t response_len);

const char *ui_nrf52840_owner_name(nrf52840_owner_t owner)
{
    switch(owner) {
    case NRF52840_OWNER_PROBE:
        return "probe";
    case NRF52840_OWNER_MESHTASTIC:
        return "meshtastic";
    case NRF52840_OWNER_BLE_SCAN:
        return "ble_scan";
    case NRF52840_OWNER_BLE_CONNECT:
        return "ble_connect";
    case NRF52840_OWNER_DFU:
        return "dfu";
    case NRF52840_OWNER_NONE:
    default:
        return "none";
    }
}

const char *ui_nrf52840_mode_name(nrf52840_ble_mode_t mode)
{
    switch(mode) {
    case NRF52840_MODE_MESHTASTIC:
        return "meshtastic";
    case NRF52840_MODE_USER_BLE:
        return "user_ble";
    case NRF52840_MODE_DFU:
        return "dfu";
    case NRF52840_MODE_OFF:
    default:
        return "off";
    }
}

static void nrf52840_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(NRF52840_MANAGER_LOG, "a");
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

static int nrf52840_version_char(int ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '+';
}

static int nrf52840_extract_at_version(const char *response,
                                       char *out, size_t out_len)
{
    const char *marker = "+VER:K230_NRF52840_AT,";
    const char *pos;
    size_t i = 0;

    if(!response || !out || out_len == 0U) {
        return -1;
    }
    out[0] = '\0';
    pos = strstr(response, marker);
    if(!pos) {
        return -1;
    }
    pos += strlen(marker);
    while(pos[i] && nrf52840_version_char((unsigned char)pos[i]) &&
          i + 1U < out_len) {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return out[0] ? 0 : -1;
}

static int nrf52840_version_supports_uart_dfu(const char *version)
{
    char lower[96];
    size_t i;

    if(!version || !version[0]) {
        return 0;
    }
    for(i = 0; version[i] && i + 1U < sizeof(lower); i++) {
        char ch = version[i];
        lower[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    lower[i] = '\0';
    return strstr(lower, "dfu") != NULL;
}

static void nrf52840_set_error_locked(const char *message)
{
    snprintf(nrf52840_status.last_error, sizeof(nrf52840_status.last_error),
             "%s", message ? message : "");
    nrf52840_status.updated_us = ui_monotonic_us();
}

static void nrf52840_mark_probe_locked(int present, int at_ok,
                                       const char *version,
                                       const char *error)
{
    int keep_cached_dfu = (!at_ok && nrf52840_status.uart_dfu_supported &&
                           nrf52840_status.version[0]);

    nrf52840_status.known = 1;
    nrf52840_status.present = present ? 1 : 0;
    nrf52840_status.at_ok = at_ok ? 1 : 0;
    if(version && version[0]) {
        snprintf(nrf52840_status.version, sizeof(nrf52840_status.version),
                 "%s", version);
        nrf52840_status.uart_dfu_supported =
            nrf52840_version_supports_uart_dfu(version);
    } else if(!at_ok) {
        if(keep_cached_dfu) {
            nrf52840_status.present = 1;
        } else {
            nrf52840_status.version[0] = '\0';
            nrf52840_status.uart_dfu_supported = 0;
        }
    }
    if(error) {
        nrf52840_set_error_locked(error);
    } else {
        nrf52840_status.last_error[0] = '\0';
        nrf52840_status.updated_us = ui_monotonic_us();
    }
}

static int nrf52840_owner_locked(void)
{
    return nrf52840_status.owner;
}

static int nrf52840_probe_now(int claim_owner, char *message,
                              size_t message_len)
{
    char response[768];
    char version[96];
    int rc;

    if(message && message_len > 0U) {
        message[0] = '\0';
    }

    if(claim_owner &&
       ui_nrf52840_request(NRF52840_OWNER_PROBE, 0) != 0) {
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s", "nRF52840 UART is busy");
        }
        return -1;
    }

    rc = nrf52840_run_at_query(response, sizeof(response));
    if(rc == 0 && strstr(response, "OK") &&
       nrf52840_extract_at_version(response, version, sizeof(version)) == 0) {
        pthread_mutex_lock(&nrf52840_lock);
        nrf52840_mark_probe_locked(1, 1, version, NULL);
        pthread_mutex_unlock(&nrf52840_lock);
        nrf52840_log("probe ok version=%s dfu=%d", version,
                     nrf52840_version_supports_uart_dfu(version));
        if(claim_owner) {
            ui_nrf52840_release(NRF52840_OWNER_PROBE);
        }
        return 0;
    }

    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_mark_probe_locked(0, 0, NULL,
                               response[0] ? response : "AT no response");
    pthread_mutex_unlock(&nrf52840_lock);
    if(message && message_len > 0U) {
        snprintf(message, message_len, "%s",
                 response[0] ? response : "nRF52840 AT response unavailable");
        ui_trim_text(message);
    }
    nrf52840_log("probe failed: %s", response[0] ? response : "no response");
    if(claim_owner) {
        ui_nrf52840_release(NRF52840_OWNER_PROBE);
    }
    return -1;
}

static void *nrf52840_probe_thread(void *arg)
{
    int attempts = (int)(intptr_t)arg;
    int i;

    if(attempts <= 0) {
        attempts = 1;
    }

    for(i = 0; i < attempts; i++) {
        int stop;
        int owner;

        pthread_mutex_lock(&nrf52840_lock);
        stop = nrf52840_shutdown_requested;
        owner = nrf52840_owner_locked();
        pthread_mutex_unlock(&nrf52840_lock);
        if(stop) {
            break;
        }
        if(owner != NRF52840_OWNER_NONE &&
           owner != NRF52840_OWNER_PROBE) {
            nrf52840_log("startup probe skipped owner=%s",
                         ui_nrf52840_owner_name((nrf52840_owner_t)owner));
        } else if(nrf52840_probe_now(1, NULL, 0) == 0) {
            break;
        }
        usleep(NRF52840_PROBE_RETRY_US);
    }

    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_probe_running = 0;
    nrf52840_next_probe_us = ui_monotonic_us() + NRF52840_PROBE_RETRY_US;
    pthread_mutex_unlock(&nrf52840_lock);
    return NULL;
}

int ui_nrf52840_refresh_async(void)
{
    pthread_t thread;
    int start = 0;
    uint64_t now = ui_monotonic_us();

    pthread_mutex_lock(&nrf52840_lock);
    if(!nrf52840_shutdown_requested && !nrf52840_probe_running &&
       now >= nrf52840_next_probe_us) {
        nrf52840_probe_running = 1;
        start = 1;
    }
    pthread_mutex_unlock(&nrf52840_lock);

    if(!start) {
        return 0;
    }
    if(pthread_create(&thread, NULL, nrf52840_probe_thread,
                      (void *)(intptr_t)1) == 0) {
        pthread_detach(thread);
        nrf52840_log("async probe scheduled");
        return 1;
    }

    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_probe_running = 0;
    nrf52840_next_probe_us = now + NRF52840_PROBE_RETRY_US;
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("async probe thread failed");
    return -1;
}

void ui_nrf52840_manager_startup(void)
{
    int start_probe = 0;

    pthread_mutex_lock(&nrf52840_lock);
    if(!nrf52840_started) {
        memset(&nrf52840_status, 0, sizeof(nrf52840_status));
        nrf52840_status.owner = NRF52840_OWNER_NONE;
        nrf52840_status.mode = NRF52840_MODE_OFF;
        nrf52840_shutdown_requested = 0;
        nrf52840_probe_running = 0;
        nrf52840_next_probe_us = 0;
        snprintf(nrf52840_status.phoneapi_state,
                 sizeof(nrf52840_status.phoneapi_state), "%s", "offline");
        nrf52840_status.updated_us = ui_monotonic_us();
        nrf52840_started = 1;
        start_probe = 1;
    }
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("startup");
    if(start_probe) {
        (void)ui_nrf52840_refresh_async();
    }
}

void ui_nrf52840_manager_shutdown(void)
{
    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_shutdown_requested = 1;
    nrf52840_status.owner = NRF52840_OWNER_NONE;
    nrf52840_status.busy = 0;
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("shutdown");
}

int ui_nrf52840_get_status(nrf52840_status_t *status)
{
    if(!status) {
        return -1;
    }
    pthread_mutex_lock(&nrf52840_lock);
    *status = nrf52840_status;
    pthread_mutex_unlock(&nrf52840_lock);
    return 0;
}

int ui_nrf52840_status_known(void)
{
    int known;

    pthread_mutex_lock(&nrf52840_lock);
    known = nrf52840_status.known;
    pthread_mutex_unlock(&nrf52840_lock);
    return known;
}

int ui_nrf52840_request(nrf52840_owner_t owner, int timeout_ms)
{
    uint64_t start = ui_monotonic_us();
    uint64_t timeout_us = timeout_ms <= 0 ? 0ULL :
                          (uint64_t)timeout_ms * 1000ULL;

    if(owner == NRF52840_OWNER_NONE) {
        return -1;
    }

    while(1) {
        pthread_mutex_lock(&nrf52840_lock);
        if(nrf52840_status.owner == NRF52840_OWNER_NONE ||
           nrf52840_status.owner == owner) {
            nrf52840_status.owner = owner;
            nrf52840_status.busy = 1;
            nrf52840_status.updated_us = ui_monotonic_us();
            pthread_mutex_unlock(&nrf52840_lock);
            nrf52840_log("owner acquired %s",
                         ui_nrf52840_owner_name(owner));
            return 0;
        }
        pthread_mutex_unlock(&nrf52840_lock);

        if(timeout_us == 0ULL || ui_monotonic_us() - start >= timeout_us) {
            nrf52840_log("owner busy requested=%s",
                         ui_nrf52840_owner_name(owner));
            return -1;
        }
        usleep(20000);
    }
}

void ui_nrf52840_release(nrf52840_owner_t owner)
{
    int released = 0;

    pthread_mutex_lock(&nrf52840_lock);
    if(nrf52840_status.owner == owner || owner == NRF52840_OWNER_NONE) {
        released = 1;
        nrf52840_status.owner = NRF52840_OWNER_NONE;
        nrf52840_status.busy = 0;
        nrf52840_status.updated_us = ui_monotonic_us();
    }
    pthread_mutex_unlock(&nrf52840_lock);

    if(released) {
        nrf52840_log("owner released %s",
                     ui_nrf52840_owner_name(owner));
    }
}

void ui_nrf52840_switch_mode(nrf52840_ble_mode_t mode)
{
    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_status.mode = mode;
    nrf52840_status.updated_us = ui_monotonic_us();
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("mode=%s", ui_nrf52840_mode_name(mode));
}

int ui_nrf52840_cached_dfu_status(char *message, size_t message_len)
{
    nrf52840_status_t status;

    if(message && message_len > 0U) {
        message[0] = '\0';
    }

    if(access(NRF52840_DFU_TOOL, X_OK) != 0) {
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s", "DFU tool missing");
        }
        return -1;
    }

    if(ui_nrf52840_get_status(&status) != 0 || !status.known) {
        (void)ui_nrf52840_refresh_async();
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s",
                     "nRF52840 status is initializing. Try again shortly.");
        }
        return -3;
    }
    if(status.uart_dfu_supported && status.version[0]) {
        return 0;
    }
    if(!status.present || !status.at_ok) {
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s",
                     status.last_error[0] ? status.last_error :
                     "nRF52840 AT firmware not detected.");
            ui_trim_text(message);
        }
        return -1;
    }
    if(!status.uart_dfu_supported) {
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s",
                     "Update the nRF52840 bootloader before using DFU.");
        }
        return -2;
    }
    return 0;
}

static nrf52840_ble_mode_t nrf52840_restore_previous_mode(void)
{
    nrf52840_ble_mode_t mode;

    pthread_mutex_lock(&nrf52840_lock);
    mode = nrf52840_dfu_previous_mode_valid ?
           nrf52840_dfu_previous_mode : NRF52840_MODE_MESHTASTIC;
    nrf52840_dfu_previous_mode_valid = 0;
    nrf52840_status.mode = mode;
    nrf52840_status.updated_us = ui_monotonic_us();
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("restore mode=%s", ui_nrf52840_mode_name(mode));
    return mode;
}

static int nrf52840_run_at_query(char *response, size_t response_len)
{
    char cmd[320];
    FILE *fp;
    size_t used = 0;
    int rc;

    if(!response || response_len == 0U) {
        return -1;
    }
    response[0] = '\0';
    if(access(NRF52840_DFU_TOOL, X_OK) != 0) {
        snprintf(response, response_len, "%s", "DFU tool missing");
        return -1;
    }
    snprintf(cmd, sizeof(cmd),
             "%s --at 'AT+VER?' --at-read-ms 1200 -p %s 2>&1",
             NRF52840_DFU_TOOL, NRF52840_UART_DEV);
    fp = popen(cmd, "r");
    if(!fp) {
        snprintf(response, response_len, "popen failed: %s", strerror(errno));
        return -1;
    }
    while(used + 1U < response_len) {
        size_t got = fread(response + used, 1, response_len - used - 1U, fp);

        used += got;
        response[used] = '\0';
        if(got == 0U) {
            break;
        }
    }
    rc = ui_shell_exit_code(pclose(fp));
    return rc == 0 ? 0 : -1;
}

int ui_nrf52840_begin_dfu(int timeout_ms, char *message, size_t message_len)
{
    nrf52840_ble_mode_t previous_mode;
    nrf52840_status_t status;

    if(message && message_len > 0U) {
        message[0] = '\0';
    }
    if(ui_nrf52840_request(NRF52840_OWNER_DFU, timeout_ms) != 0) {
        if(ui_nrf52840_get_status(&status) == 0 &&
           status.owner == NRF52840_OWNER_MESHTASTIC) {
            nrf52840_log("force pause meshtastic for dfu");
            if(system("killall " NRF52840_MESHTASTIC_PROC " >/dev/null 2>&1 || true") == -1) {
                nrf52840_log("killall meshtastic failed");
            }
            ui_nrf52840_release(NRF52840_OWNER_MESHTASTIC);
            usleep(300000);
        }
        if(ui_nrf52840_request(NRF52840_OWNER_DFU, timeout_ms) != 0) {
            if(message && message_len > 0U) {
                snprintf(message, message_len, "%s", "nRF52840 is busy");
            }
            return -1;
        }
    }

    pthread_mutex_lock(&nrf52840_lock);
    previous_mode = nrf52840_status.mode;
    if(previous_mode != NRF52840_MODE_DFU) {
        nrf52840_dfu_previous_mode = previous_mode;
        nrf52840_dfu_previous_mode_valid = 1;
    }
    pthread_mutex_unlock(&nrf52840_lock);

    ui_nrf52840_switch_mode(NRF52840_MODE_DFU);
    nrf52840_log("pause meshtastic for dfu previous=%s",
                 ui_nrf52840_mode_name(previous_mode));
    if(system("killall " NRF52840_MESHTASTIC_PROC " >/dev/null 2>&1 || true") == -1) {
        nrf52840_log("killall meshtastic failed");
    }
    usleep(300000);
    return 0;
}

int ui_nrf52840_probe_at(char *message, size_t message_len)
{
    return nrf52840_probe_now(0, message, message_len);
}

int ui_nrf52840_prepare_dfu(char *message, size_t message_len)
{
    nrf52840_status_t status;
    int rc;

    if(ui_nrf52840_begin_dfu(1800, message, message_len) != 0) {
        return -1;
    }

    rc = ui_nrf52840_probe_at(message, message_len);
    ui_nrf52840_get_status(&status);
    if(rc == 0 && !status.uart_dfu_supported) {
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s",
                     "Update the nRF52840 bootloader before using DFU.");
        }
        rc = -2;
    }

    (void)nrf52840_restore_previous_mode();
    ui_nrf52840_release(NRF52840_OWNER_DFU);
    return rc;
}

void ui_nrf52840_note_dfu_finished(int rc)
{
    nrf52840_ble_mode_t mode = nrf52840_restore_previous_mode();

    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_status.mode = mode;
    nrf52840_status.busy = 0;
    nrf52840_status.owner = NRF52840_OWNER_NONE;
    if(rc != 0) {
        nrf52840_set_error_locked("DFU failed");
    } else {
        nrf52840_status.last_error[0] = '\0';
        nrf52840_status.updated_us = ui_monotonic_us();
    }
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("dfu finished rc=%d", rc);
}

void ui_nrf52840_note_phoneapi_state(const char *state)
{
    const char *value = state && state[0] ? state : "offline";
    int mode_can_follow;

    pthread_mutex_lock(&nrf52840_lock);
    mode_can_follow = nrf52840_status.owner == NRF52840_OWNER_NONE ||
                      nrf52840_status.owner == NRF52840_OWNER_MESHTASTIC;
    snprintf(nrf52840_status.phoneapi_state,
             sizeof(nrf52840_status.phoneapi_state), "%s", value);
    if(mode_can_follow) {
        nrf52840_status.mode = NRF52840_MODE_MESHTASTIC;
    }
    nrf52840_status.phoneapi_connected =
        strcasecmp(value, "connected") == 0;
    if(strcasecmp(value, "ready") == 0 ||
       strcasecmp(value, "connected") == 0 ||
       strcasecmp(value, "probing") == 0) {
        nrf52840_status.known = 1;
        nrf52840_status.present = 1;
        nrf52840_status.at_ok = 1;
        nrf52840_status.last_error[0] = '\0';
    } else if(strcasecmp(value, "unsupported") == 0 ||
              strcasecmp(value, "error") == 0) {
        nrf52840_status.known = 1;
        nrf52840_status.present = 1;
        nrf52840_status.at_ok = 0;
        nrf52840_set_error_locked(value);
    }
    nrf52840_status.updated_us = ui_monotonic_us();
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("phoneapi=%s", value);
}

void ui_nrf52840_note_custom_ble_state(int ready, int connected,
                                       const char *detail)
{
    pthread_mutex_lock(&nrf52840_lock);
    nrf52840_status.known = 1;
    nrf52840_status.present = ready ? 1 : nrf52840_status.present;
    nrf52840_status.at_ok = ready ? 1 : nrf52840_status.at_ok;
    nrf52840_status.phoneapi_connected = connected ? 1 : 0;
    nrf52840_status.mode = NRF52840_MODE_USER_BLE;
    if(detail && detail[0]) {
        snprintf(nrf52840_status.last_error,
                 sizeof(nrf52840_status.last_error), "%s", detail);
    } else if(ready) {
        nrf52840_status.last_error[0] = '\0';
    }
    nrf52840_status.updated_us = ui_monotonic_us();
    pthread_mutex_unlock(&nrf52840_lock);
    nrf52840_log("custom_ble ready=%d connected=%d %s", ready, connected,
                 detail ? detail : "");
}
