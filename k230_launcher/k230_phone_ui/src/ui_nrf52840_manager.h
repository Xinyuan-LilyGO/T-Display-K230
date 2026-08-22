#ifndef K230_PHONE_UI_NRF52840_MANAGER_H
#define K230_PHONE_UI_NRF52840_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF52840_OWNER_NONE = 0,
    NRF52840_OWNER_MESHTASTIC,
    NRF52840_OWNER_BLE_SCAN,
    NRF52840_OWNER_BLE_CONNECT,
    NRF52840_OWNER_DFU,
} nrf52840_owner_t;

typedef enum {
    NRF52840_MODE_OFF = 0,
    NRF52840_MODE_MESHTASTIC,
    NRF52840_MODE_USER_BLE,
    NRF52840_MODE_DFU,
} nrf52840_ble_mode_t;

typedef struct {
    int known;
    int present;
    int at_ok;
    int uart_dfu_supported;
    int phoneapi_connected;
    int busy;
    nrf52840_owner_t owner;
    nrf52840_ble_mode_t mode;
    char version[96];
    char phoneapi_state[32];
    char last_error[160];
    uint64_t updated_us;
} nrf52840_status_t;

void ui_nrf52840_manager_startup(void);
void ui_nrf52840_manager_shutdown(void);
int ui_nrf52840_get_status(nrf52840_status_t *status);
int ui_nrf52840_status_known(void);

int ui_nrf52840_request(nrf52840_owner_t owner, int timeout_ms);
void ui_nrf52840_release(nrf52840_owner_t owner);
void ui_nrf52840_switch_mode(nrf52840_ble_mode_t mode);

int ui_nrf52840_begin_dfu(int timeout_ms, char *message, size_t message_len);
int ui_nrf52840_probe_at(char *message, size_t message_len);
int ui_nrf52840_prepare_dfu(char *message, size_t message_len);
void ui_nrf52840_note_dfu_finished(int rc);
void ui_nrf52840_note_phoneapi_state(const char *state);
void ui_nrf52840_note_custom_ble_state(int ready, int connected,
                                       const char *detail);

const char *ui_nrf52840_owner_name(nrf52840_owner_t owner);
const char *ui_nrf52840_mode_name(nrf52840_ble_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
