#include "ui_hardware.h"

#include "ui_audio.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PREF_AUDIO_OUTPUT "audio.output"
#define PREF_FAN_MODE "fan.mode"
#define PREF_FAN_DUTY "fan.duty"
#define PREF_FAN_ON_TEMP "fan.on_temp_c"
#define PREF_FAN_OFF_TEMP "fan.off_temp_c"
#define PREF_BQ25896_ICHG_MA "bq25896.ichg_ma"
#define PREF_KEYBOARD_BACKLIGHT "keyboard.backlight_pct"
#define PREF_KEYBOARD_BACKLIGHT_PWM_HZ "keyboard.backlight_pwm_hz"
#define PREF_EXTENSION_KEYBOARD "keyboard.extension_enabled"
#define PREF_EXTENSION_KEYBOARD_AUTO_DETECT "keyboard.auto_detect"
#define PREF_EXTENSION_KEYBOARD_AUTO_INTERVAL "keyboard.auto_detect_interval_s"
#define PREF_EXTENSION_KEYBOARD_AUTO_ROTATE "keyboard.auto_rotate_display"
#define PREF_EXTENSION_KEYBOARD_ESC_BACK "keyboard.esc_back_enabled"
#define PREF_EXTENSION_KEYBOARD_HOTKEY_PREFIX "keyboard.hotkey.f"

#define AUDIO_OUTPUT_HEADPHONES "headphones"
#define AUDIO_OUTPUT_EXTERNAL "external"
#define AUDIO_EXTERNAL_I2S_CONTROL "External I2S Output Switch"

#define FAN_MODE_OFF "off"
#define FAN_MODE_ON "on"
#define FAN_MODE_AUTO "auto"

#define AMP_SHUTDOWN_GPIO 34U
#define FAN_GPIO 42U
#define BUTTON_BOOT0_GPIO 0U
#define BUTTON_INT0_GPIO 64U
#define BUTTON_BOOT0_IDLE_VALUE 1
#define BUTTON_INT0_IDLE_VALUE 0
#define BUTTON_TEST_LOG "/tmp/k230_button_test.log"
#define REBOOT_DIAG_LOG "/tmp/k230_reboot_diag.log"
#define KEYBOARD_BACKLIGHT_LOG "/tmp/k230_keyboard_backlight.log"
#define BOOT0_TOGGLE_DEBOUNCE_US 500000ULL
#define BOOT0_FADE_STEPS 12
#define BOOT0_FADE_STEP_US 25000
#define HARDWARE_BACKLIGHT_PATH_MAX 160
#define BUTTON_BOOT0_IOMUX_IO0_OFFSET 0U
#define BUTTON_BOOT0_IOMUX_GPIO_INPUT_VALUE 0x00000344U
#define BUTTON_INT0_PMU_IOMUX_BASE 0x91000080UL
#define BUTTON_INT0_PMU_IO0_OFFSET 0U
#define BUTTON_IOMUX_DI_MASK 0x80000000U
#define INT0_PMU_BASE_ADDR 0x91000000UL
#define INT0_PMU_IO_SIZE 0x00000C00UL
#define INT0_PWR_BASE_ADDR 0x91103000UL
#define INT0_PWR_IO_SIZE 0x00001000UL
#define INT0_PMU_INT0_TO_CPU_REGISTER 0x48U
#define INT0_PMU_INT_DETECT_EN 0x4CU
#define INT0_PMU_INT_DETECT_TYP 0x50U
#define INT0_PMU_INT_DETECT_CLR 0x54U
#define INT0_PMU_INT0_LONG_PRESS_TRIGGER_VAL 0x58U
#define INT0_PMU_INT0_LEVEL_DEBOUNCE_VAL 0x64U
#define INT0_PMU_INT_STATE_REG 0xACU
#define INT0_PMU_PWR_ISO_CTRL_REG 0x158U
#define INT0_PMU_ISO_ACCESS_MASK (1U << 5)
#define INT0_PMU_CPU_IRQ_MASK 0x0FFFU
#define INT0_PMU_IRQ_KEY_LONG (1U << 11)
#define INT0_PMU_IRQ_KEY_SHORT (1U << 10)
#define INT0_PMU_IRQ_KEY_EDGE (1U << 9)
#define INT0_PMU_IRQ_KEY_SHUTDOWN (1U << 0)
#define INT0_PMU_DET_KEY_LONG (1U << 11)
#define INT0_PMU_DET_KEY_SHORT (1U << 10)
#define INT0_PMU_DET_KEY_EDGE (1U << 9)
#define INT0_PMU_CLR_KEY_LONG 0x0200U
#define INT0_PMU_CLR_KEY_SHUTDOWN 0x0100U
#define INT0_PMU_CLR_KEY_SHORT 0x0080U
#define INT0_PMU_CLR_KEY_EDGE 0x0040U
#define INT0_PMU_INT_TRIGGER_MASK 0x7U
#define INT0_PMU_INT_TRIGGER_TYPE_MASK 0x1U
#define INT0_PMU_INT_TRIGGER_EDGE_MASK 0x2U
#define INT0_PMU_KEY_EDGE_OFFSET 16U
#define INT0_PMU_PWRKEY_LONG_PRESS_TICKS 96000U
#define INT0_PMU_PWRKEY_DEBOUNCE_TICKS 256U
#define KEYBOARD_CLICK_RAW_PATH "/tmp/k230_keyboard_click.raw"
#define KEYBOARD_CLICK_LOG_PATH "/tmp/k230_keyboard_click.log"
#define KEYBOARD_CLICK_RATE 48000
#define AHT20_ADDR 0x38
#define AHT20_LINUX_I2C_BUS 0
#define AHT20_STATUS_BUSY 0x80
#define AHT20_STATUS_CALIBRATED 0x08
#define AHT20_IOMUX_BASE 0x91105000UL
#define AHT20_IOMUX_SIZE 0x1000UL
#define AHT20_IOMUX_IO46_OFFSET (46U * 4U)
#define AHT20_IOMUX_IO47_OFFSET (47U * 4U)
#define AHT20_GPIO_CHIP "/dev/gpiochip1"
#define AHT20_GPIO_SCL_OFFSET 14U
#define AHT20_GPIO_SDA_OFFSET 15U
#define KEYBOARD_BACKLIGHT_IOMUX_IO52_OFFSET (52U * 4U)
#define KEYBOARD_BACKLIGHT_IOMUX_PWM4_VALUE 0x00001191U
#define TCA8418_IRQ_IOMUX_IO42_OFFSET (42U * 4U)
#define TCA8418_IRQ_IOMUX_GPIO_INPUT_VALUE 0x00000344U
#define KEYBOARD_BASE_I2C_LABEL "I2C4 SDA47/SCL46"
#define BQ25896_ADDR 0x6B
#define BQ25896_REG_INPUT 0x00
#define BQ25896_REG_PWR_ONOFF 0x02
#define BQ25896_REG_CHG_CTRL 0x03
#define BQ25896_REG_CHG_CURRENT 0x04
#define BQ25896_REG_CHG_VOLT 0x06
#define BQ25896_REG_STATUS 0x0B
#define BQ25896_REG_FAULT 0x0C
#define BQ25896_REG_ADC_BATV 0x0E
#define BQ25896_REG_ADC_SYSV 0x0F
#define BQ25896_REG_ADC_NTC 0x10
#define BQ25896_REG_ADC_BUSV 0x11
#define BQ25896_REG_ADC_ICHGR 0x12
#define BQ25896_REG_IDPM 0x13
#define BQ25896_REG_DEVICE_REV 0x14
#define BQ25896_MASK_CONV_START 0x80
#define BQ25896_MASK_CHG_CONFIG 0x10
#define BQ25896_MASK_ICHG 0x7F
#define BQ25896_MASK_VREG 0xFC
#define BQ25896_FAST_CHG_STEP_MA 64
#define BQ25896_FAST_CHG_MAX_MA 3008
#define BQ25896_INPUT_CURRENT_BASE_MA 100
#define BQ25896_INPUT_CURRENT_STEP_MA 50
#define BQ25896_CHG_VOLT_BASE_MV 3840
#define BQ25896_CHG_VOLT_STEP_MV 16
#define BQ25896_ADC_VOLT_BASE_MV 2304
#define BQ25896_ADC_VBAT_STEP_MV 20
#define BQ25896_ADC_VSYS_STEP_MV 20
#define BQ25896_ADC_VBUS_BASE_MV 2600
#define BQ25896_ADC_VBUS_STEP_MV 100
#define BQ25896_ADC_ICHG_STEP_MA 50
#define BQ25896_ADC_NTC_BASE_PCT 21.0
#define BQ25896_ADC_NTC_STEP_PCT 0.465
#define BQ27220_ADDR 0x55
#define BQ27220_REG_TEMP 0x06
#define BQ27220_REG_VOLTAGE 0x08
#define BQ27220_REG_BATTERY_STATUS 0x0A
#define BQ27220_REG_CURRENT 0x0C
#define BQ27220_REG_REMAINING_CAPACITY 0x10
#define BQ27220_REG_FULL_CHARGE_CAPACITY 0x12
#define BQ27220_REG_TIME_TO_EMPTY 0x16
#define BQ27220_REG_TIME_TO_FULL 0x18
#define BQ27220_REG_STANDBY_CURRENT 0x1A
#define BQ27220_REG_MAX_LOAD_CURRENT 0x1E
#define BQ27220_REG_AVERAGE_POWER 0x24
#define BQ27220_REG_INTERNAL_TEMP 0x28
#define BQ27220_REG_CYCLE_COUNT 0x2A
#define BQ27220_REG_SOC 0x2C
#define BQ27220_REG_SOH 0x2E
#define BQ27220_REG_CHARGING_VOLTAGE 0x30
#define BQ27220_REG_CHARGING_CURRENT 0x32
#define BQ27220_REG_OPERATION_STATUS 0x3A
#define BQ27220_REG_DESIGN_CAPACITY 0x3C
#define TCA8418_ADDR 0x34
#define TCA8418_ROWS 7
#define TCA8418_COLS 10
#define TCA8418_RST_GPIO 43U
#define TCA8418_IRQ_GPIO 42U
#define TCA8418_REG_CFG 0x01
#define TCA8418_REG_INT_STAT 0x02
#define TCA8418_REG_KEY_LCK_EC 0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_KP_GPIO_1 0x1D
#define TCA8418_REG_KP_GPIO_2 0x1E
#define TCA8418_REG_KP_GPIO_3 0x1F
#define TCA8418_REG_DEBOUNCE_DIS_1 0x29
#define TCA8418_REG_DEBOUNCE_DIS_2 0x2A
#define TCA8418_REG_DEBOUNCE_DIS_3 0x2B
#define TCA8418_CFG_KE_IEN 0x01
#define TCA8418_CFG_OVR_FLOW_IEN 0x08
#define TCA8418_CFG_OVR_FLOW_M 0x20
#ifndef K230_TCA8418_USE_IRQ
#define K230_TCA8418_USE_IRQ 1
#endif
#if K230_TCA8418_USE_IRQ
#define TCA8418_CFG_MODE (TCA8418_CFG_KE_IEN | TCA8418_CFG_OVR_FLOW_IEN | \
                          TCA8418_CFG_OVR_FLOW_M)
#else
#define TCA8418_CFG_MODE TCA8418_CFG_OVR_FLOW_M
#endif
#define TCA8418_STAT_K_INT 0x01
#define TCA8418_STAT_K_LCK_INT 0x04
#define TCA8418_STAT_OVR_FLOW_INT 0x08
#define XL9555_ADDR 0x20
#define XL9555_REG_INPUT0 0x00
#define XL9555_REG_OUTPUT0 0x02
#define XL9555_REG_CONFIG0 0x06
#define XL9555_LED_P03_BIT 3U
#define XL9555_LED_P04_BIT 4U
#define XL9555_LED_P05_BIT 5U
#define XL9555_LED_MASK ((uint8_t)((1U << XL9555_LED_P03_BIT) | \
                                   (1U << XL9555_LED_P04_BIT) | \
                                   (1U << XL9555_LED_P05_BIT)))
#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif
#define FAN_PWM_CHIP "/sys/class/pwm/pwmchip0"
#define FAN_PWM_PERIOD_NS 100000
#define KEYBOARD_BACKLIGHT_GPIO 52U
#define KEYBOARD_BACKLIGHT_PWM_DEFAULT_HZ 50000
#define KEYBOARD_BACKLIGHT_PWM_MIN_HZ 20000
#define KEYBOARD_BACKLIGHT_PWM_MAX_HZ 1000000
#define KEYBOARD_BACKLIGHT_DEFAULT_PCT 0
#define EXT_KEY_AUTO_DETECT_DEFAULT 1
#define EXT_KEY_AUTO_INTERVAL_DEFAULT_S 3
#define EXT_KEY_AUTO_INTERVAL_MIN_S 3
#define EXT_KEY_AUTO_INTERVAL_MAX_S 30
#define KEYBOARD_LAYOUT_ROWS 6
#define KEYBOARD_LAYOUT_COLS 11
#define KEYBOARD_HOTKEY_FKEY_COUNT 11
#define EXT_KEY_QUEUE_SIZE 64
#define TCA8418_RAW_QUEUE_SIZE 96
#define EXT_KEY_FAIL_LIMIT 5
#define EXT_KEY_REPEAT_START_US 450000ULL
#define EXT_KEY_REPEAT_PERIOD_US 85000ULL

typedef struct {
    const char *chip;
    unsigned int channel;
} pwm_sysfs_target_t;

typedef struct {
    int ok;
    int bus;
    double temp_c;
    double humidity_pct;
    char source[64];
    char status[96];
} sensor_reading_t;

static int sensor_read_aht20(sensor_reading_t *reading);

typedef struct {
    int scanned;
    int bq25896;
    int bq27220;
    int tca8418;
    int xl9555;
    char status[128];
} keyboard_base_state_t;

typedef struct {
    int ok;
    int present;
    int revision;
    int part_number;
    int charge_enabled;
    int fast_charge_ma;
    int input_limit_ma;
    int charge_voltage_mv;
    int vbat_mv;
    int vsys_mv;
    int vbus_mv;
    int charge_current_ma;
    double ntc_pct;
    uint8_t status_reg;
    uint8_t fault_reg;
    uint8_t idpm_reg;
    char status[128];
} bq25896_reading_t;

typedef struct {
    int ok;
    int present;
    double temp_c;
    double internal_temp_c;
    int voltage_mv;
    int current_ma;
    int remaining_mah;
    int full_charge_mah;
    int design_mah;
    int soc_pct;
    int soh_pct;
    int time_to_empty_min;
    int time_to_full_min;
    int standby_current_ma;
    int max_load_current_ma;
    int average_power_mw;
    int cycle_count;
    int charging_voltage_mv;
    int charging_current_ma;
    uint16_t battery_status;
    uint16_t operation_status;
    char status[128];
} bq27220_reading_t;

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} extension_key_event_t;

typedef struct {
    int code;
    int pressed;
} tca8418_raw_event_t;

typedef enum {
    KEYBOARD_HOTKEY_NONE = 0,
    KEYBOARD_HOTKEY_HOME,
    KEYBOARD_HOTKEY_SETTINGS,
    KEYBOARD_HOTKEY_TERMINAL,
    KEYBOARD_HOTKEY_MESHTASTIC,
    KEYBOARD_HOTKEY_CAMERA,
    KEYBOARD_HOTKEY_SCREENSHOT,
    KEYBOARD_HOTKEY_ROTATE_NEXT,
    KEYBOARD_HOTKEY_ROTATE_0,
    KEYBOARD_HOTKEY_ROTATE_90,
    KEYBOARD_HOTKEY_ROTATE_180,
    KEYBOARD_HOTKEY_ROTATE_270,
    KEYBOARD_HOTKEY_VOLUME_DOWN,
    KEYBOARD_HOTKEY_VOLUME_UP,
    KEYBOARD_HOTKEY_KEYBOARD_BACKLIGHT,
    KEYBOARD_HOTKEY_COUNT
} keyboard_hotkey_action_t;

typedef struct {
    int f_index;
    int code;
    const char *name;
    keyboard_hotkey_action_t fallback;
} keyboard_hotkey_fkey_t;

typedef struct {
    keyboard_hotkey_action_t action;
    const char *label;
} keyboard_hotkey_action_def_t;

static pthread_t hardware_thread;
static int hardware_thread_started;
static int hardware_thread_stop;
static pthread_mutex_t hardware_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sensor_aht20_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t keyboard_base_lock = PTHREAD_MUTEX_INITIALIZER;
static keyboard_base_state_t keyboard_base_state;
static int sensor_aht20_force_gpio;
static int fan_auto_running;
static int fan_last_duty = -1;
static int keyboard_backlight_last = -1;
static int keyboard_backlight_freq_last = -1;
static int keyboard_backlight_suppress_ui_update;
static char keyboard_backlight_status[96] = "Not applied";
static struct gpiod_chip *amp_gpio_chip;
static struct gpiod_line_request *amp_gpio_request;
static unsigned int amp_gpio_offset;
static int amp_gpio_enabled = -1;
static int audio_external_route = -1;
static int audio_external_route_supported = -1;
static pthread_mutex_t audio_input_route_lock = PTHREAD_MUTEX_INITIALIZER;
static int audio_input_route_refcount;
static int audio_input_route_restore_external;
static int audio_input_route_restore_valid;

static lv_obj_t *audio_status_label;
static lv_obj_t *audio_headphones_btn;
static lv_obj_t *audio_external_btn;
static lv_obj_t *audio_settings_volume_slider;
static lv_obj_t *audio_settings_volume_label;
static lv_obj_t *fan_status_label;
static lv_obj_t *fan_temp_label;
static lv_obj_t *fan_mode_label;
static lv_obj_t *fan_duty_label;
static lv_obj_t *fan_on_label;
static lv_obj_t *fan_off_label;
static lv_obj_t *fan_off_btn;
static lv_obj_t *fan_on_btn;
static lv_obj_t *fan_auto_btn;
static lv_obj_t *fan_duty_slider;
static lv_obj_t *fan_on_slider;
static lv_obj_t *fan_off_slider;
static lv_obj_t *sensor_status_label;
static lv_obj_t *sensor_temp_label;
static lv_obj_t *sensor_humidity_label;
static lv_obj_t *sensor_bus_label;
static lv_obj_t *sensor_cpu_label;
static lv_obj_t *bq25896_status_label;
static lv_obj_t *bq25896_base_label;
static lv_obj_t *bq25896_charge_label;
static lv_obj_t *bq25896_slider_label;
static lv_obj_t *bq25896_input_label;
static lv_obj_t *bq25896_voltage_label;
static lv_obj_t *bq25896_vbat_label;
static lv_obj_t *bq25896_vsys_label;
static lv_obj_t *bq25896_vbus_label;
static lv_obj_t *bq25896_ichg_label;
static lv_obj_t *bq25896_ntc_label;
static lv_obj_t *bq25896_fault_label;
static lv_obj_t *bq25896_charge_on_btn;
static lv_obj_t *bq25896_charge_off_btn;
static lv_obj_t *bq25896_current_slider;
static lv_obj_t *battery_status_label;
static lv_obj_t *battery_base_label;
static lv_obj_t *battery_soc_label;
static lv_obj_t *battery_soc_bar;
static lv_obj_t *battery_voltage_label;
static lv_obj_t *battery_current_label;
static lv_obj_t *battery_remaining_label;
static lv_obj_t *battery_full_label;
static lv_obj_t *battery_design_label;
static lv_obj_t *battery_temp_label;
static lv_obj_t *battery_internal_temp_label;
static lv_obj_t *battery_time_empty_label;
static lv_obj_t *battery_time_full_label;
static lv_obj_t *battery_health_label;
static lv_obj_t *battery_cycle_label;
static lv_obj_t *battery_power_label;
static lv_obj_t *battery_status_reg_label;
static lv_obj_t *keyboard_status_label;
static lv_obj_t *keyboard_base_label;
static lv_obj_t *keyboard_last_label;
static lv_obj_t *keyboard_count_label;
static lv_obj_t *keyboard_backlight_label;
static lv_obj_t *keyboard_backlight_slider;
static lv_obj_t *keyboard_key_box[KEYBOARD_LAYOUT_ROWS][KEYBOARD_LAYOUT_COLS];
static lv_obj_t *keyboard_key_label[KEYBOARD_LAYOUT_ROWS][KEYBOARD_LAYOUT_COLS];
static lv_obj_t *button_boot0_state_label;
static lv_obj_t *button_boot0_raw_label;
static lv_obj_t *button_int0_state_label;
static lv_obj_t *button_int0_raw_label;
static lv_obj_t *button_status_label;
static lv_obj_t *int0_test_state_label;
static lv_obj_t *int0_test_raw_label;
static lv_obj_t *int0_test_counts_label;
static lv_obj_t *int0_test_log_label;
static lv_obj_t *int0_test_reset_btn;
static lv_obj_t *xl9555_status_label;
static lv_obj_t *xl9555_base_label;
static lv_obj_t *xl9555_output_label;
static lv_obj_t *xl9555_config_label;
static lv_obj_t *xl9555_led_btn[3];
static int boot0_toggle_initialized;
static int boot0_last_raw = BUTTON_BOOT0_IDLE_VALUE;
static int boot0_screen_off;
static int boot0_saved_backlight = -1;
static int boot0_saved_keyboard_backlight = -1;
static uint64_t boot0_last_toggle_us;
static pthread_mutex_t tca8418_event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t tca8418_irq_thread;
static int tca8418_irq_thread_started;
static int tca8418_irq_stop;
static int tca8418_irq_ready;
static unsigned int tca8418_irq_count;
static unsigned int tca8418_irq_error_count;
static unsigned int tca8418_irq_safety_count;
static unsigned int tca8418_overflow_count;
static uint64_t tca8418_irq_last_us;
static int tca8418_last_fifo_count;
static int tca8418_reset_dispatch_requested;
static struct gpiod_chip *tca8418_irq_chip;
static struct gpiod_line_request *tca8418_irq_request;
static tca8418_raw_event_t tca8418_raw_queue[TCA8418_RAW_QUEUE_SIZE];
static int tca8418_raw_head;
static int tca8418_raw_tail;
static int keyboard_key_pressed[TCA8418_ROWS][TCA8418_COLS];
static int keyboard_tca8418_ready;
static unsigned int keyboard_event_total;
static char keyboard_last_event_text[128] = "No event";
static uint64_t keyboard_last_init_us;
static lv_indev_t *extension_keyboard_indev;
static lv_group_t *extension_keyboard_group;
static extension_key_event_t extension_key_queue[EXT_KEY_QUEUE_SIZE];
static int extension_key_head;
static int extension_key_tail;
static int extension_keyboard_requested;
static int extension_keyboard_active;
static int extension_keyboard_fail_count;
static int extension_keyboard_caps;
static int extension_keyboard_pinyin;
static int extension_keyboard_repeat_code;
static uint32_t extension_keyboard_repeat_key;
static uint64_t extension_keyboard_repeat_next_us;
static uint64_t extension_keyboard_next_auto_probe_us;
static int keyboard_test_sound_enabled;
static int keyboard_click_raw_ready;
static uint64_t keyboard_last_click_us;
static char extension_keyboard_status[128] = "Extension keyboard off";
static ui_extension_keyboard_key_cb_t extension_keyboard_key_cb;
static void *extension_keyboard_key_user_data;
static int xl9555_ready;
static int xl9555_led_state[3];
static lv_timer_t *hardware_page_timer;
static lv_obj_t *keyboard_settings_status_label;
static lv_obj_t *keyboard_settings_auto_switch;
static lv_obj_t *keyboard_settings_esc_back_switch;
static lv_obj_t *keyboard_settings_interval_btn[3];
static lv_obj_t *keyboard_settings_hotkey_btn[KEYBOARD_HOTKEY_FKEY_COUNT];

static void style_choice_button(lv_obj_t *btn, int selected, uint32_t accent);
static int keyboard_backlight_pref_frequency_hz(void);
static int keyboard_backlight_pwm_update_duty_only(int duty_percent);
static void keyboard_backlight_apply(int duty_percent, int frequency_hz,
                                     int save_pref);
static int xl9555_set_led(int index, int enabled);
static uint32_t extension_keyboard_shift_symbol_for_code(int code);
static void keyboard_settings_update_ui(void);

static const keyboard_hotkey_fkey_t keyboard_hotkey_fkeys[] = {
    { 1, 50, "F1", KEYBOARD_HOTKEY_HOME },
    { 2, 60, "F2", KEYBOARD_HOTKEY_SETTINGS },
    { 3, 59, "F3", KEYBOARD_HOTKEY_ROTATE_NEXT },
    { 4, 68, "F4", KEYBOARD_HOTKEY_SCREENSHOT },
    { 5, 67, "F5", KEYBOARD_HOTKEY_VOLUME_DOWN },
    { 6, 66, "F6", KEYBOARD_HOTKEY_VOLUME_UP },
    { 7, 65, "F7", KEYBOARD_HOTKEY_KEYBOARD_BACKLIGHT },
    { 8, 64, "F8", KEYBOARD_HOTKEY_TERMINAL },
    { 9, 63, "F9", KEYBOARD_HOTKEY_MESHTASTIC },
    { 10, 62, "F10", KEYBOARD_HOTKEY_CAMERA },
    { 11, 61, "F11", KEYBOARD_HOTKEY_NONE },
};

static const keyboard_hotkey_action_def_t keyboard_hotkey_actions[] = {
    { KEYBOARD_HOTKEY_NONE, "None" },
    { KEYBOARD_HOTKEY_HOME, "Home" },
    { KEYBOARD_HOTKEY_SETTINGS, "Settings" },
    { KEYBOARD_HOTKEY_TERMINAL, "Terminal" },
    { KEYBOARD_HOTKEY_MESHTASTIC, "Meshtastic" },
    { KEYBOARD_HOTKEY_CAMERA, "Camera" },
    { KEYBOARD_HOTKEY_SCREENSHOT, "Screenshot" },
    { KEYBOARD_HOTKEY_ROTATE_NEXT, "Rotate next" },
    { KEYBOARD_HOTKEY_ROTATE_0, "Rotate 0" },
    { KEYBOARD_HOTKEY_ROTATE_90, "Rotate 90" },
    { KEYBOARD_HOTKEY_ROTATE_180, "Rotate 180" },
    { KEYBOARD_HOTKEY_ROTATE_270, "Rotate 270" },
    { KEYBOARD_HOTKEY_VOLUME_DOWN, "Volume down" },
    { KEYBOARD_HOTKEY_VOLUME_UP, "Volume up" },
    { KEYBOARD_HOTKEY_KEYBOARD_BACKLIGHT, "Keyboard backlight" },
};

static void extension_keyboard_refresh_async(void *user_data)
{
    (void)user_data;
    app_refresh_current_page();
}

static void extension_keyboard_rotate_270_async(void *user_data)
{
    (void)user_data;
    if(app_display_rotation_degrees() != 270) {
        app_set_display_rotation_degrees(270);
    }
}

static int clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static int read_pref_int(const char *key, int fallback, int min_value,
                         int max_value)
{
    char value[32];
    int parsed;

    ui_prefs_get(key, value, sizeof(value), "");
    if(!value[0]) {
        return clamp_int(fallback, min_value, max_value);
    }
    parsed = atoi(value);
    return clamp_int(parsed, min_value, max_value);
}

static void write_pref_int(const char *key, int value)
{
    char text[32];

    snprintf(text, sizeof(text), "%d", value);
    ui_prefs_set(key, text);
}

static void read_pref_text(const char *key, char *buf, size_t len,
                           const char *fallback)
{
    if(!buf || len == 0) {
        return;
    }
    ui_prefs_get(key, buf, len, fallback ? fallback : "");
    if(!buf[0] && fallback) {
        snprintf(buf, len, "%s", fallback);
    }
}

static int write_text_file(const char *path, const char *text)
{
    int fd;
    size_t len;
    ssize_t written;

    if(!path || !text) {
        return -1;
    }

    fd = open(path, O_WRONLY);
    if(fd < 0) {
        return -1;
    }

    len = strlen(text);
    written = write(fd, text, len);
    close(fd);
    return written == (ssize_t)len ? 0 : -1;
}

static int read_double_from_file(const char *path, double scale, double *value)
{
    char text[64];

    if(!value || ui_read_file_first_line(path, text, sizeof(text)) != 0) {
        return -1;
    }
    *value = atof(text) / scale;
    return 0;
}

static int gpio_set_value_once(unsigned int gpio, int value,
                               const char *consumer)
{
    struct gpiod_chip *chip;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_config;
    struct gpiod_request_config *request_config;
    struct gpiod_line_request *request;
    unsigned int chip_index = gpio / 32U;
    unsigned int offset = gpio % 32U;
    char chip_path[32];
    int ok = -1;

    if(chip_index > 1U) {
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
        goto out_alloc;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_AS_IS);
    gpiod_line_settings_set_output_value(settings, value ?
                                         GPIOD_LINE_VALUE_ACTIVE :
                                         GPIOD_LINE_VALUE_INACTIVE);
    gpiod_request_config_set_consumer(request_config,
                                      consumer ? consumer : "k230-phone-ui");

    if(gpiod_line_config_add_line_settings(line_config, &offset, 1,
                                           settings) != 0) {
        goto out_alloc;
    }

    request = gpiod_chip_request_lines(chip, request_config, line_config);
    if(!request) {
        goto out_alloc;
    }
    ok = gpiod_line_request_set_value(request, offset, value ?
                                      GPIOD_LINE_VALUE_ACTIVE :
                                      GPIOD_LINE_VALUE_INACTIVE);
    usleep(1000);
    gpiod_line_request_release(request);

out_alloc:
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_config);
    gpiod_request_config_free(request_config);
    gpiod_chip_close(chip);
    return ok;
}

static void button_test_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(BUTTON_TEST_LOG, "a");
    if(!fp) {
        return;
    }
    va_start(ap, fmt);
    fprintf(fp, "%llu ", (unsigned long long)ui_monotonic_us());
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

static void keyboard_backlight_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(KEYBOARD_BACKLIGHT_LOG, "a");
    if(!fp) {
        return;
    }
    va_start(ap, fmt);
    fprintf(fp, "%llu ", (unsigned long long)ui_monotonic_us());
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

static int gpio_get_value_from_path_offset(const char *chip_path,
                                           unsigned int offset, int *value,
                                           const char *consumer,
                                           char *err, size_t err_len)
{
    struct gpiod_chip *chip = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    struct gpiod_line_request *request = NULL;
    enum gpiod_line_value line_value;
    int rc = -1;

    if(!chip_path || !value) {
        return -1;
    }

    chip = gpiod_chip_open(chip_path);
    if(!chip) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "%s open failed: %s", chip_path,
                     strerror(errno));
        }
        return -1;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if(!settings || !line_config || !request_config) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "%s alloc failed", chip_path);
        }
        goto out_alloc;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_AS_IS);
    gpiod_request_config_set_consumer(request_config,
                                      consumer ? consumer : "k230-phone-button");

    if(gpiod_line_config_add_line_settings(line_config, &offset, 1,
                                           settings) != 0) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "%s offset%u config failed: %s",
                     chip_path, offset, strerror(errno));
        }
        goto out_alloc;
    }

    request = gpiod_chip_request_lines(chip, request_config, line_config);
    if(!request) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "%s offset%u request failed: %s",
                     chip_path, offset, strerror(errno));
        }
        goto out_alloc;
    }
    line_value = gpiod_line_request_get_value(request, offset);
    if(line_value == GPIOD_LINE_VALUE_ACTIVE ||
       line_value == GPIOD_LINE_VALUE_INACTIVE) {
        *value = line_value == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0;
        rc = 0;
    } else if(err && err_len > 0) {
        snprintf(err, err_len, "%s offset%u invalid value", chip_path, offset);
    }
    gpiod_line_request_release(request);

out_alloc:
    if(settings) {
        gpiod_line_settings_free(settings);
    }
    if(line_config) {
        gpiod_line_config_free(line_config);
    }
    if(request_config) {
        gpiod_request_config_free(request_config);
    }
    if(chip) {
        gpiod_chip_close(chip);
    }
    return rc;
}

static int button_read_int0_pmuiomux(int *value, char *source,
                                     size_t source_len, char *err,
                                     size_t err_len)
{
    int fd;
    void *map;
    volatile uint8_t *bytes;
    uint32_t reg;
    long page_size;
    unsigned long page_mask;
    unsigned long map_base;
    size_t map_offset;
    size_t map_size;

    if(!value) {
        return -1;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if(page_size <= 0) {
        page_size = 4096;
    }
    page_mask = (unsigned long)page_size - 1UL;
    map_base = BUTTON_INT0_PMU_IOMUX_BASE & ~page_mask;
    map_offset = (size_t)(BUTTON_INT0_PMU_IOMUX_BASE - map_base);
    map_size = map_offset + BUTTON_INT0_PMU_IO0_OFFSET + sizeof(uint32_t);
    if(map_size < (size_t)page_size) {
        map_size = (size_t)page_size;
    }

    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if(fd < 0) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "pmuiomux open /dev/mem failed: %s",
                     strerror(errno));
        }
        return -1;
    }

    map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, (off_t)map_base);
    close(fd);
    if(map == MAP_FAILED) {
        if(err && err_len > 0) {
            snprintf(err, err_len,
                     "pmuiomux mmap 0x%08lx+0x%zx failed: %s",
                     map_base, map_size, strerror(errno));
        }
        return -1;
    }

    bytes = (volatile uint8_t *)map;
    reg = *(volatile uint32_t *)(bytes + map_offset +
                                 BUTTON_INT0_PMU_IO0_OFFSET);
    *value = (reg & BUTTON_IOMUX_DI_MASK) ? 1 : 0;
    if(source && source_len > 0) {
        snprintf(source, source_len, "pmuiomux:io0.DI=0x%08x", reg);
    }
    munmap(map, map_size);
    return 0;
}

static int button_read_gpio(unsigned int gpio, int *value,
                            char *source, size_t source_len,
                            char *err, size_t err_len)
{
    static const unsigned int int0_candidates[][2] = {
        {2U, 0U},
    };
    unsigned int chip_index = gpio / 32U;
    unsigned int offset = gpio % 32U;
    char chip_path[32];
    char local_err[128] = "";
    char pmu_err[160] = "";

    if(source && source_len > 0) {
        source[0] = '\0';
    }
    if(err && err_len > 0) {
        err[0] = '\0';
    }

    if(gpio == BUTTON_INT0_GPIO &&
       button_read_int0_pmuiomux(value, source, source_len, pmu_err,
                                 sizeof(pmu_err)) == 0) {
        if(err && err_len > 0) {
            err[0] = '\0';
        }
        return 0;
    }
    if(gpio == BUTTON_INT0_GPIO && err && err_len > 0 && pmu_err[0]) {
        snprintf(err, err_len, "%s", pmu_err);
    }

    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u", chip_index);
    if(gpio_get_value_from_path_offset(chip_path, offset, value,
                                       "k230-phone-buttons", local_err,
                                       sizeof(local_err)) == 0) {
        if(source && source_len > 0) {
            snprintf(source, source_len, "%s:%u", chip_path, offset);
        }
        return 0;
    }
    if(err && err_len > 0) {
        size_t used = strlen(err);

        if(used < err_len) {
            snprintf(err + used, err_len - used, "%s%s", used ? " | " : "",
                     local_err);
        }
    }

    if(gpio != BUTTON_INT0_GPIO) {
        return -1;
    }

    if(access("/dev/gpiochip2", F_OK) != 0 && err && err_len > 0) {
        size_t used = strlen(err);

        if(used < err_len) {
            snprintf(err + used, err_len - used, "%sGPIO64 is not exposed by "
                     "the current kernel gpiochips",
                     used ? " | " : "");
        }
    }

    for(size_t i = 0; i < sizeof(int0_candidates) / sizeof(int0_candidates[0]); i++) {
        unsigned int candidate_chip = int0_candidates[i][0];
        unsigned int candidate_offset = int0_candidates[i][1];

        if(candidate_chip == chip_index && candidate_offset == offset) {
            continue;
        }
        snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u",
                 candidate_chip);
        local_err[0] = '\0';
        if(gpio_get_value_from_path_offset(chip_path, candidate_offset, value,
                                           "k230-phone-buttons", local_err,
                                           sizeof(local_err)) == 0) {
            if(source && source_len > 0) {
                snprintf(source, source_len, "%s:%u", chip_path,
                         candidate_offset);
            }
            return 0;
        }
        if(err && err_len > 0 && local_err[0]) {
            size_t used = strlen(err);

            if(used < err_len) {
                snprintf(err + used, err_len - used, "%s%s",
                         used ? " | " : "", local_err);
            }
        }
    }

    return -1;
}

static int button_log_index(unsigned int gpio)
{
    if(gpio == BUTTON_BOOT0_GPIO) {
        return 0;
    }
    if(gpio == BUTTON_INT0_GPIO) {
        return 1;
    }
    return -1;
}

static int button_prepare_boot0_gpio(void)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        button_test_log("BOOT0 iomux open /dev/mem failed: %s",
                        strerror(errno));
        return -1;
    }

    map = mmap(NULL, AHT20_IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               AHT20_IOMUX_BASE);
    close(fd);
    if(map == MAP_FAILED) {
        button_test_log("BOOT0 iomux mmap failed: %s", strerror(errno));
        return -1;
    }

    regs = (volatile uint32_t *)map;
    regs[BUTTON_BOOT0_IOMUX_IO0_OFFSET / 4U] =
        BUTTON_BOOT0_IOMUX_GPIO_INPUT_VALUE;
    usleep(1000);
    button_test_log("BOOT0 iomux set IO0=GPIO0 input pull-up reg=0x%08x",
                    regs[BUTTON_BOOT0_IOMUX_IO0_OFFSET / 4U]);
    munmap(map, AHT20_IOMUX_SIZE);
    return 0;
}

static int tca8418_prepare_irq_gpio(void)
{
#if K230_TCA8418_USE_IRQ
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        button_test_log("TCA8418 IRQ iomux open /dev/mem failed: %s",
                        strerror(errno));
        return -1;
    }

    map = mmap(NULL, AHT20_IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               AHT20_IOMUX_BASE);
    close(fd);
    if(map == MAP_FAILED) {
        button_test_log("TCA8418 IRQ iomux mmap failed: %s", strerror(errno));
        return -1;
    }

    regs = (volatile uint32_t *)map;
    regs[TCA8418_IRQ_IOMUX_IO42_OFFSET / 4U] =
        TCA8418_IRQ_IOMUX_GPIO_INPUT_VALUE;
    usleep(1000);
    button_test_log("TCA8418 IRQ iomux set IO42=GPIO42 input pull-up reg=0x%08x",
                    regs[TCA8418_IRQ_IOMUX_IO42_OFFSET / 4U]);
    munmap(map, AHT20_IOMUX_SIZE);
    return 0;
#else
    return 0;
#endif
}

static int hardware_find_backlight(char *brightness, size_t brightness_len)
{
    DIR *dir;
    struct dirent *ent;

    if(!brightness || brightness_len == 0) {
        return -1;
    }
    brightness[0] = '\0';

    dir = opendir("/sys/class/backlight");
    if(!dir) {
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {
        char path[HARDWARE_BACKLIGHT_PATH_MAX];

        if(ent->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
                 ent->d_name);
        if(ui_path_exists(path)) {
            snprintf(brightness, brightness_len, "%s", path);
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return -1;
}

static int hardware_read_backlight_raw(void)
{
    char path[HARDWARE_BACKLIGHT_PATH_MAX];
    char text[32];

    if(hardware_find_backlight(path, sizeof(path)) != 0 ||
       ui_read_file_first_line(path, text, sizeof(text)) != 0) {
        return -1;
    }
    return atoi(text);
}

static int hardware_write_backlight_raw(int value)
{
    char path[HARDWARE_BACKLIGHT_PATH_MAX];
    FILE *fp;

    if(value < 0) {
        value = 0;
    }
    if(hardware_find_backlight(path, sizeof(path)) != 0) {
        return -1;
    }
    fp = fopen(path, "w");
    if(!fp) {
        return -1;
    }
    fprintf(fp, "%d\n", value);
    fclose(fp);
    return 0;
}

static int keyboard_backlight_current_or_pref(void)
{
    int duty;

    pthread_mutex_lock(&hardware_lock);
    duty = keyboard_backlight_last;
    pthread_mutex_unlock(&hardware_lock);
    if(duty < 0) {
        duty = read_pref_int(PREF_KEYBOARD_BACKLIGHT,
                             KEYBOARD_BACKLIGHT_DEFAULT_PCT, 0, 100);
    }
    return clamp_int(duty, 0, 100);
}

static void boot0_fade_backlights(int screen_from, int screen_to,
                                  int keyboard_from, int keyboard_to)
{
    int frequency_hz = keyboard_backlight_pref_frequency_hz();
    int keyboard_should_drive;

    screen_from = screen_from < 0 ? screen_to : screen_from;
    screen_to = screen_to < 0 ? screen_from : screen_to;
    keyboard_from = clamp_int(keyboard_from, 0, 100);
    keyboard_to = clamp_int(keyboard_to, 0, 100);
    keyboard_should_drive = keyboard_from > 0 || keyboard_to > 0;

    keyboard_backlight_suppress_ui_update = 1;
    for(int step = 1; step <= BOOT0_FADE_STEPS; step++) {
        int screen_value = screen_from +
            ((screen_to - screen_from) * step) / BOOT0_FADE_STEPS;
        int keyboard_value = keyboard_from +
            ((keyboard_to - keyboard_from) * step) / BOOT0_FADE_STEPS;

        hardware_write_backlight_raw(screen_value);
        if(keyboard_should_drive) {
            keyboard_backlight_apply(keyboard_value, frequency_hz, 0);
        }
        usleep(BOOT0_FADE_STEP_US);
    }
    hardware_write_backlight_raw(screen_to);
    if(screen_to == 0) {
        usleep(20000);
        hardware_write_backlight_raw(0);
    }
    if(keyboard_should_drive) {
        keyboard_backlight_apply(keyboard_to, frequency_hz, 0);
    }
    if(!keyboard_should_drive) {
        pthread_mutex_lock(&hardware_lock);
        keyboard_backlight_last = 0;
        snprintf(keyboard_backlight_status, sizeof(keyboard_backlight_status),
                 "Keyboard backlight unchanged off during BOOT0 fade");
        pthread_mutex_unlock(&hardware_lock);
        keyboard_backlight_log("boot0-fade screen=%d->%d keyboard=0 skip=already-off",
                               screen_from, screen_to);
    }
    keyboard_backlight_suppress_ui_update = 0;
}

static void boot0_apply_screen_toggle(int turn_off)
{
    int brightness;
    int keyboard_brightness;

    if(turn_off) {
        brightness = hardware_read_backlight_raw();
        if(brightness > 0) {
            boot0_saved_backlight = brightness;
        } else if(boot0_saved_backlight <= 0) {
            boot0_saved_backlight = 80;
        }
        keyboard_brightness = keyboard_backlight_current_or_pref();
        boot0_saved_keyboard_backlight = keyboard_brightness;
        boot0_fade_backlights(brightness > 0 ? brightness : boot0_saved_backlight,
                              0, keyboard_brightness, 0);
        boot0_screen_off = 1;
        button_test_log("BOOT0 toggled display off saved_bl=%d saved_kbd=%d",
                        boot0_saved_backlight,
                        boot0_saved_keyboard_backlight);
    } else {
        brightness = boot0_saved_backlight > 0 ? boot0_saved_backlight : 80;
        keyboard_brightness = boot0_saved_keyboard_backlight >= 0 ?
                              boot0_saved_keyboard_backlight :
                              read_pref_int(PREF_KEYBOARD_BACKLIGHT,
                                            KEYBOARD_BACKLIGHT_DEFAULT_PCT,
                                            0, 100);
        boot0_fade_backlights(hardware_read_backlight_raw(), brightness,
                              keyboard_backlight_current_or_pref(),
                              keyboard_brightness);
        boot0_screen_off = 0;
        button_test_log("BOOT0 toggled display on restore_bl=%d restore_kbd=%d",
                        brightness, boot0_saved_keyboard_backlight);
    }
}

int ui_hardware_screen_backlight_get(void)
{
    return hardware_read_backlight_raw();
}

int ui_hardware_keyboard_backlight_get(void)
{
    return keyboard_backlight_current_or_pref();
}

int ui_hardware_boot0_screen_off(void)
{
    int raw = hardware_read_backlight_raw();

    if(boot0_screen_off && raw > 16) {
        boot0_screen_off = 0;
        button_test_log("BOOT0 screen-off state cleared because screen backlight is on raw=%d",
                        raw);
    }
    return boot0_screen_off;
}

void ui_hardware_set_screen_off(int off)
{
    int current = ui_hardware_boot0_screen_off();

    if((off ? 1 : 0) == current) {
        return;
    }
    boot0_apply_screen_toggle(off ? 1 : 0);
}

void ui_hardware_shutdown_backlights_apply(int screen_value,
                                           int keyboard_percent)
{
    int frequency_hz = keyboard_backlight_pref_frequency_hz();
    int keyboard_last;

    screen_value = clamp_int(screen_value, 0, 10000);
    keyboard_percent = clamp_int(keyboard_percent, 0, 100);

    keyboard_backlight_suppress_ui_update = 1;
    hardware_write_backlight_raw(screen_value);
    pthread_mutex_lock(&hardware_lock);
    keyboard_last = keyboard_backlight_last;
    pthread_mutex_unlock(&hardware_lock);
    if(keyboard_percent <= 0 && keyboard_last <= 0) {
        pthread_mutex_lock(&hardware_lock);
        keyboard_backlight_last = 0;
        snprintf(keyboard_backlight_status, sizeof(keyboard_backlight_status),
                 "Keyboard backlight already off");
        pthread_mutex_unlock(&hardware_lock);
        keyboard_backlight_log("shutdown-apply screen=%d keyboard=0 skip=already-off",
                               screen_value);
    } else {
        keyboard_backlight_apply(keyboard_percent, frequency_hz, 0);
    }
    keyboard_backlight_suppress_ui_update = 0;
    keyboard_backlight_log("shutdown-apply screen=%d keyboard=%d freq=%d",
                           screen_value, keyboard_percent, frequency_hz);
}

void ui_hardware_shutdown_backlights_step_apply(int screen_value,
                                                int keyboard_percent)
{
    int rc;
    static int last_logged_keyboard = -1;
    static int last_logged_screen = -1;
    int keyboard_last;

    screen_value = clamp_int(screen_value, 0, 10000);
    keyboard_percent = clamp_int(keyboard_percent, 0, 100);

    keyboard_backlight_suppress_ui_update = 1;
    hardware_write_backlight_raw(screen_value);
    pthread_mutex_lock(&hardware_lock);
    keyboard_last = keyboard_backlight_last;
    pthread_mutex_unlock(&hardware_lock);
    if(keyboard_percent <= 0 && keyboard_last <= 0) {
        pthread_mutex_lock(&hardware_lock);
        keyboard_backlight_last = 0;
        snprintf(keyboard_backlight_status, sizeof(keyboard_backlight_status),
                 "Keyboard backlight already off");
        pthread_mutex_unlock(&hardware_lock);
        if(last_logged_keyboard != 0 ||
           abs(screen_value - last_logged_screen) >= 5) {
            keyboard_backlight_log("shutdown-step screen=%d keyboard=0 skip=already-off",
                                   screen_value);
            last_logged_keyboard = 0;
            last_logged_screen = screen_value;
        }
        keyboard_backlight_suppress_ui_update = 0;
        return;
    }
    rc = keyboard_backlight_pwm_update_duty_only(keyboard_percent);
    if(rc != 0) {
        keyboard_backlight_apply(keyboard_percent,
                                 keyboard_backlight_pref_frequency_hz(), 0);
        keyboard_backlight_log("shutdown-step screen=%d keyboard=%d duty_only_rc=%d fallback=1",
                               screen_value, keyboard_percent, rc);
    } else {
        pthread_mutex_lock(&hardware_lock);
        keyboard_backlight_last = keyboard_percent;
        snprintf(keyboard_backlight_status, sizeof(keyboard_backlight_status),
                 "PWM duty-only shutdown");
        pthread_mutex_unlock(&hardware_lock);
        if(last_logged_keyboard < 0 || keyboard_percent <= 10 ||
           keyboard_percent == 0 ||
           abs(keyboard_percent - last_logged_keyboard) >= 5 ||
           abs(screen_value - last_logged_screen) >= 5) {
            keyboard_backlight_log("shutdown-step screen=%d keyboard=%d duty_only_rc=0 fallback=0",
                                   screen_value, keyboard_percent);
            last_logged_keyboard = keyboard_percent;
            last_logged_screen = screen_value;
        }
    }
    keyboard_backlight_suppress_ui_update = 0;
}

static void boot0_toggle_poll(void)
{
    char source[40];
    char err[160];
    int value;
    uint64_t now = ui_monotonic_us();

    if(button_read_gpio(BUTTON_BOOT0_GPIO, &value, source, sizeof(source),
                        err, sizeof(err)) != 0) {
        return;
    }

    if(!boot0_toggle_initialized) {
        boot0_last_raw = value;
        boot0_toggle_initialized = 1;
        if(hardware_read_backlight_raw() <= 0) {
            boot0_screen_off = 1;
            button_test_log("BOOT0 initial screen-off state from backlight=0");
        }
        return;
    }

    if(boot0_last_raw == BUTTON_BOOT0_IDLE_VALUE &&
       value != BUTTON_BOOT0_IDLE_VALUE &&
       now - boot0_last_toggle_us > BOOT0_TOGGLE_DEBOUNCE_US) {
        boot0_last_toggle_us = now;
        boot0_apply_screen_toggle(!boot0_screen_off);
    }
    boot0_last_raw = value;
}

static int amp_gpio_request_line(void)
{
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_config;
    struct gpiod_request_config *request_config;
    unsigned int chip_index = AMP_SHUTDOWN_GPIO / 32U;
    char chip_path[32];
    int ok = -1;

    if(amp_gpio_request) {
        return 0;
    }

    if(chip_index > 1U) {
        return -1;
    }

    amp_gpio_offset = AMP_SHUTDOWN_GPIO % 32U;
    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u", chip_index);
    amp_gpio_chip = gpiod_chip_open(chip_path);
    if(!amp_gpio_chip) {
        return -1;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if(!settings || !line_config || !request_config) {
        goto out_alloc;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_AS_IS);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);
    gpiod_request_config_set_consumer(request_config, "k230-phone-amp");

    if(gpiod_line_config_add_line_settings(line_config, &amp_gpio_offset, 1,
                                           settings) != 0) {
        goto out_alloc;
    }

    amp_gpio_request = gpiod_chip_request_lines(amp_gpio_chip, request_config,
                                                line_config);
    ok = amp_gpio_request ? 0 : -1;

out_alloc:
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_config);
    gpiod_request_config_free(request_config);
    if(ok != 0) {
        if(amp_gpio_chip) {
            gpiod_chip_close(amp_gpio_chip);
            amp_gpio_chip = NULL;
        }
    }
    return ok;
}

int ui_amp_set_enabled(int enabled)
{
    int value = enabled ? 1 : 0;
    int rc;

    if(amp_gpio_request_line() != 0) {
        return -1;
    }

    rc = gpiod_line_request_set_value(amp_gpio_request, amp_gpio_offset,
                                      value ? GPIOD_LINE_VALUE_ACTIVE :
                                      GPIOD_LINE_VALUE_INACTIVE);
    if(rc == 0) {
        amp_gpio_enabled = value;
    }
    return rc;
}

int ui_amp_is_enabled(void)
{
    return amp_gpio_enabled > 0;
}

static int audio_set_external_i2s_route(int external)
{
    char cmd[192];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "amixer -q cset name='%s' %d >/dev/null 2>&1",
             AUDIO_EXTERNAL_I2S_CONTROL, external ? 1 : 0);
    rc = system(cmd);
    if(rc == 0) {
        audio_external_route = external ? 1 : 0;
        audio_external_route_supported = 1;
        return 0;
    }

    audio_external_route_supported = 0;
    return -1;
}

int ui_audio_output_set_external(int external)
{
    int route_rc;
    int amp_rc;

    route_rc = audio_set_external_i2s_route(external ? 1 : 0);
    amp_rc = ui_amp_set_enabled(external ? 1 : 0);

    return (route_rc == 0 && amp_rc == 0) ? 0 : -1;
}

int ui_audio_output_is_external(void)
{
    return audio_external_route > 0 || amp_gpio_enabled > 0;
}

int ui_audio_input_route_enter(const char *owner)
{
    int rc = 0;

    pthread_mutex_lock(&audio_input_route_lock);
    if(audio_input_route_refcount == 0) {
        audio_input_route_restore_external = ui_audio_output_is_external();
        audio_input_route_restore_valid = 1;
        rc = ui_audio_output_set_external(0);
        fprintf(stderr,
                "[audio-route] %s enter input route, saved=%s rc=%d\n",
                owner ? owner : "capture",
                audio_input_route_restore_external ? "external" : "headphones",
                rc);
    }
    audio_input_route_refcount++;
    pthread_mutex_unlock(&audio_input_route_lock);
    return rc;
}

void ui_audio_input_route_leave(const char *owner)
{
    int restore = 0;
    int valid = 0;
    int rc = 0;

    pthread_mutex_lock(&audio_input_route_lock);
    if(audio_input_route_refcount > 0) {
        audio_input_route_refcount--;
        if(audio_input_route_refcount == 0 && audio_input_route_restore_valid) {
            restore = audio_input_route_restore_external;
            valid = 1;
            audio_input_route_restore_valid = 0;
        }
    }
    pthread_mutex_unlock(&audio_input_route_lock);

    if(valid) {
        rc = ui_audio_output_set_external(restore);
        fprintf(stderr,
                "[audio-route] %s leave input route, restored=%s rc=%d\n",
                owner ? owner : "capture",
                restore ? "external" : "headphones", rc);
    }
}

static void amp_gpio_release(void)
{
    if(amp_gpio_request) {
        gpiod_line_request_release(amp_gpio_request);
        amp_gpio_request = NULL;
    }
    if(amp_gpio_chip) {
        gpiod_chip_close(amp_gpio_chip);
        amp_gpio_chip = NULL;
    }
    amp_gpio_enabled = -1;
}

static int pwm_channel_path(const pwm_sysfs_target_t *target, char *path,
                            size_t len)
{
    if(!target || !target->chip || !path || len == 0) {
        return -1;
    }
    snprintf(path, len, "%s/pwm%u", target->chip, target->channel);
    return 0;
}

static int pwm_export_if_needed(const pwm_sysfs_target_t *target)
{
    char pwm_path[128];
    char export_path[128];
    char text[24];

    if(pwm_channel_path(target, pwm_path, sizeof(pwm_path)) != 0) {
        return -1;
    }
    snprintf(export_path, sizeof(export_path), "%s/export", target->chip);
    snprintf(text, sizeof(text), "%u", target->channel);

    if(ui_path_exists(pwm_path)) {
        return 0;
    }
    if(write_text_file(export_path, text) != 0 && errno != EBUSY) {
        return -1;
    }
    for(int i = 0; i < 10; i++) {
        if(ui_path_exists(pwm_path)) {
            return 0;
        }
        usleep(20000);
    }
    return ui_path_exists(pwm_path) ? 0 : -1;
}

static int pwm_set_percent(const pwm_sysfs_target_t *target, int duty_percent,
                           int period_ns)
{
    char pwm_path[128];
    char file_path[160];
    char text[64];
    int duty_ns;

    duty_percent = clamp_int(duty_percent, 0, 100);
    if(pwm_channel_path(target, pwm_path, sizeof(pwm_path)) != 0 ||
       pwm_export_if_needed(target) != 0) {
        return -1;
    }

    snprintf(file_path, sizeof(file_path), "%s/enable", pwm_path);
    write_text_file(file_path, "0");
    snprintf(file_path, sizeof(file_path), "%s/duty_cycle", pwm_path);
    write_text_file(file_path, "0");
    snprintf(file_path, sizeof(file_path), "%s/period", pwm_path);
    snprintf(text, sizeof(text), "%d", period_ns);
    if(write_text_file(file_path, text) != 0) {
        return -1;
    }
    /*
     * The K230 PWM sysfs driver rejects "normal" polarity. Keep the supported
     * inversed mode and invert duty in software so UI percentage stays
     * active-high: 0% = low, 100% = high.
     */
    snprintf(file_path, sizeof(file_path), "%s/polarity", pwm_path);
    write_text_file(file_path, "inversed");

    duty_ns = (period_ns * (100 - duty_percent)) / 100;
    snprintf(file_path, sizeof(file_path), "%s/duty_cycle", pwm_path);
    snprintf(text, sizeof(text), "%d", duty_ns);
    if(write_text_file(file_path, text) != 0) {
        return -1;
    }

    snprintf(file_path, sizeof(file_path), "%s/enable", pwm_path);
    return write_text_file(file_path, "1");
}

static int pwm_set_duty(int duty_percent)
{
    const pwm_sysfs_target_t fan_pwm = {
        FAN_PWM_CHIP,
        0
    };

    return pwm_set_percent(&fan_pwm, duty_percent, FAN_PWM_PERIOD_NS);
}

static void fan_apply_duty(int duty_percent)
{
    if(!K230_FAN_ENABLED) {
        pthread_mutex_lock(&hardware_lock);
        fan_last_duty = 0;
        fan_auto_running = 0;
        pthread_mutex_unlock(&hardware_lock);
        return;
    }

    duty_percent = clamp_int(duty_percent, 0, 100);
    pthread_mutex_lock(&hardware_lock);
    if(fan_last_duty == duty_percent) {
        pthread_mutex_unlock(&hardware_lock);
        return;
    }
    fan_last_duty = duty_percent;
    pthread_mutex_unlock(&hardware_lock);

    if(pwm_set_duty(duty_percent) != 0) {
        gpio_set_value_once(FAN_GPIO, duty_percent > 0 ? 1 : 0,
                            "k230-phone-fan");
    }
}

static int keyboard_backlight_pref_frequency_hz(void)
{
    return KEYBOARD_BACKLIGHT_PWM_DEFAULT_HZ;
}

static int keyboard_backlight_period_from_hz(int frequency_hz)
{
    frequency_hz = clamp_int(frequency_hz,
                             KEYBOARD_BACKLIGHT_PWM_MIN_HZ,
                             KEYBOARD_BACKLIGHT_PWM_MAX_HZ);
    return 1000000000 / frequency_hz;
}

static int keyboard_backlight_pwm_set(int duty_percent, char *status,
                                      size_t status_len, int frequency_hz)
{
    static const pwm_sysfs_target_t targets[] = {
        { "/sys/class/pwm/pwmchip3", 1 },
        { "/sys/class/pwm/pwmchip1", 1 },
        { "/sys/class/pwm/pwmchip0", 4 },
        { "/sys/class/pwm/pwmchip1", 4 },
    };
    DIR *dir;
    int period_ns;
    int saw_pwm = 0;

    frequency_hz = clamp_int(frequency_hz,
                             KEYBOARD_BACKLIGHT_PWM_MIN_HZ,
                             KEYBOARD_BACKLIGHT_PWM_MAX_HZ);
    period_ns = keyboard_backlight_period_from_hz(frequency_hz);

    dir = opendir("/sys/class/pwm");
    if(dir) {
        struct dirent *entry;

        while((entry = readdir(dir)) != NULL) {
            char chip_path[128];
            char name_path[160];
            char name[32];
            pwm_sysfs_target_t target;

            if(strncmp(entry->d_name, "pwmchip", 7) != 0) {
                continue;
            }

            snprintf(chip_path, sizeof(chip_path), "/sys/class/pwm/%s",
                     entry->d_name);
            snprintf(name_path, sizeof(name_path), "%s/device/of_node/name",
                     chip_path);
            if(ui_read_file_first_line(name_path, name, sizeof(name)) != 0 ||
               strcmp(name, "pwm3_5") != 0) {
                continue;
            }

            saw_pwm = 1;
            target.chip = chip_path;
            target.channel = 1;
            if(pwm_set_percent(&target, duty_percent, period_ns) == 0) {
                if(status && status_len > 0) {
                    snprintf(status, status_len, "PWM %s/pwm%u %dHz",
                             target.chip, target.channel, frequency_hz);
                }
                closedir(dir);
                return 0;
            }
        }
        closedir(dir);
    }

    for(size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        char pwm_path[128];
        int rc;

        if(pwm_channel_path(&targets[i], pwm_path, sizeof(pwm_path)) == 0 &&
           ui_path_exists(pwm_path)) {
            saw_pwm = 1;
        }
        rc = pwm_set_percent(&targets[i], duty_percent, period_ns);
        if(pwm_channel_path(&targets[i], pwm_path, sizeof(pwm_path)) == 0 &&
           ui_path_exists(pwm_path)) {
            saw_pwm = 1;
        }
        if(rc == 0) {
            if(status && status_len > 0) {
                snprintf(status, status_len, "PWM %s/pwm%u %dHz",
                         targets[i].chip, targets[i].channel, frequency_hz);
            }
            return 0;
        }
    }
    return saw_pwm ? -2 : -1;
}

static int keyboard_backlight_pwm_write_duty_only(
    const pwm_sysfs_target_t *target, int duty_percent)
{
    char pwm_path[128];
    char file_path[160];
    char text[64];
    int period_ns;
    int duty_ns;

    duty_percent = clamp_int(duty_percent, 0, 100);

    if(pwm_channel_path(target, pwm_path, sizeof(pwm_path)) != 0 ||
       !ui_path_exists(pwm_path)) {
        return -1;
    }

    snprintf(file_path, sizeof(file_path), "%s/period", pwm_path);
    if(ui_read_file_first_line(file_path, text, sizeof(text)) != 0) {
        return -1;
    }
    period_ns = atoi(text);
    if(period_ns <= 0) {
        period_ns = keyboard_backlight_period_from_hz(
            KEYBOARD_BACKLIGHT_PWM_DEFAULT_HZ);
    }

    duty_ns = (period_ns * (100 - duty_percent)) / 100;
    snprintf(file_path, sizeof(file_path), "%s/duty_cycle", pwm_path);
    snprintf(text, sizeof(text), "%d", duty_ns);
    return write_text_file(file_path, text);
}

static int keyboard_backlight_pwm_update_duty_only(int duty_percent)
{
    static const pwm_sysfs_target_t targets[] = {
        { "/sys/class/pwm/pwmchip3", 1 },
        { "/sys/class/pwm/pwmchip1", 1 },
        { "/sys/class/pwm/pwmchip0", 4 },
        { "/sys/class/pwm/pwmchip1", 4 },
    };
    DIR *dir;
    int rc = -1;

    duty_percent = clamp_int(duty_percent, 0, 100);

    dir = opendir("/sys/class/pwm");
    if(dir) {
        struct dirent *entry;

        while((entry = readdir(dir)) != NULL) {
            char chip_path[128];
            char name_path[160];
            char name[32];
            pwm_sysfs_target_t target;

            if(strncmp(entry->d_name, "pwmchip", 7) != 0) {
                continue;
            }

            snprintf(chip_path, sizeof(chip_path), "/sys/class/pwm/%s",
                     entry->d_name);
            snprintf(name_path, sizeof(name_path), "%s/device/of_node/name",
                     chip_path);
            if(ui_read_file_first_line(name_path, name, sizeof(name)) != 0 ||
               strcmp(name, "pwm3_5") != 0) {
                continue;
            }

            target.chip = chip_path;
            target.channel = 1;
            rc = keyboard_backlight_pwm_write_duty_only(&target,
                                                        duty_percent);
            break;
        }
        closedir(dir);
    }

    if(rc == 0) {
        return 0;
    }

    for(size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        rc = keyboard_backlight_pwm_write_duty_only(&targets[i],
                                                   duty_percent);
        if(rc == 0) {
            return 0;
        }
    }

    return -1;
}

static int keyboard_backlight_iomux_pwm4(void)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        return -1;
    }

    map = mmap(NULL, AHT20_IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               AHT20_IOMUX_BASE);
    close(fd);
    if(map == MAP_FAILED) {
        return -1;
    }

    regs = (volatile uint32_t *)map;
    regs[KEYBOARD_BACKLIGHT_IOMUX_IO52_OFFSET / 4U] =
        KEYBOARD_BACKLIGHT_IOMUX_PWM4_VALUE;
    usleep(1000);
    munmap(map, AHT20_IOMUX_SIZE);
    return 0;
}

static void keyboard_backlight_update_ui(void)
{
    int duty;
    char text[128];

    pthread_mutex_lock(&hardware_lock);
    duty = keyboard_backlight_last;
    pthread_mutex_unlock(&hardware_lock);

    if(duty < 0) {
        duty = read_pref_int(PREF_KEYBOARD_BACKLIGHT,
                             KEYBOARD_BACKLIGHT_DEFAULT_PCT, 0, 100);
    }

    if(keyboard_backlight_label) {
        snprintf(text, sizeof(text), "%d%%", duty);
        lv_label_set_text(keyboard_backlight_label, text);
    }
    if(keyboard_backlight_slider) {
        lv_slider_set_value(keyboard_backlight_slider, duty, LV_ANIM_OFF);
    }
}

static void keyboard_backlight_apply(int duty_percent, int frequency_hz,
                                     int save_pref)
{
    char status[96] = "Not applied";
    int mux_rc;
    int rc;

    duty_percent = clamp_int(duty_percent, 0, 100);
    if(frequency_hz <= 0) {
        pthread_mutex_lock(&hardware_lock);
        frequency_hz = keyboard_backlight_freq_last;
        pthread_mutex_unlock(&hardware_lock);
        if(frequency_hz <= 0) {
            frequency_hz = keyboard_backlight_pref_frequency_hz();
        }
    }
    frequency_hz = KEYBOARD_BACKLIGHT_PWM_DEFAULT_HZ;
    if(save_pref) {
        write_pref_int(PREF_KEYBOARD_BACKLIGHT, duty_percent);
        write_pref_int(PREF_KEYBOARD_BACKLIGHT_PWM_HZ, frequency_hz);
    }

    mux_rc = keyboard_backlight_iomux_pwm4();
    rc = keyboard_backlight_pwm_set(duty_percent, status, sizeof(status),
                                    frequency_hz);
    if(rc == 0) {
        size_t used = strlen(status);

        if(used < sizeof(status)) {
            snprintf(status + used, sizeof(status) - used, "%s",
                     mux_rc == 0 ? " mux IO52" : " mux unknown");
        }
    }
    if(rc != 0) {
        if(rc == -1) {
            rc = gpio_set_value_once(KEYBOARD_BACKLIGHT_GPIO,
                                     duty_percent > 0 ? 1 : 0,
                                     "k230-phone-kbd-bl");
            snprintf(status, sizeof(status), "%s",
                     rc == 0 ? "GPIO52 fallback" : "GPIO52 unavailable");
        } else {
            snprintf(status, sizeof(status), "PWM apply failed");
        }
    }

    pthread_mutex_lock(&hardware_lock);
    keyboard_backlight_last = duty_percent;
    keyboard_backlight_freq_last = frequency_hz;
    snprintf(keyboard_backlight_status, sizeof(keyboard_backlight_status),
             "%s", status);
    pthread_mutex_unlock(&hardware_lock);
    keyboard_backlight_log("apply duty=%d freq=%d save=%d mux_rc=%d rc=%d status=%s",
                           duty_percent, frequency_hz, save_pref, mux_rc, rc,
                           status);

    if(!keyboard_backlight_suppress_ui_update) {
        keyboard_backlight_update_ui();
    }
}

static int sensor_normalize_cpu_temp(const char *text, double *temp_c)
{
    char *end = NULL;
    double raw;
    double abs_raw;
    double temp;

    if(!text || !temp_c) {
        return -1;
    }

    raw = strtod(text, &end);
    if(end == text) {
        return -1;
    }

    abs_raw = raw < 0.0 ? -raw : raw;
    if(abs_raw > 1000.0 && abs_raw < 200000.0) {
        temp = raw / 1000.0;
    } else if(abs_raw > 150.0 && abs_raw < 15000.0) {
        temp = raw / 100.0;
    } else {
        temp = raw;
    }

    if(temp < -50.0 || temp > 180.0) {
        return -1;
    }

    *temp_c = temp;
    return 0;
}

static int sensor_read_cpu_temp_path(const char *path, double *temp_c)
{
    char text[64];

    if(ui_read_file_first_line(path, text, sizeof(text)) != 0) {
        return -1;
    }

    return sensor_normalize_cpu_temp(text, temp_c);
}

static bool sensor_thermal_type_is_k230(const char *type)
{
    return type && (strstr(type, "canaan") || strstr(type, "k230") ||
                    strstr(type, "tsensor"));
}

static int sensor_read_cpu_temp(double *temp_c)
{
    char type_path[96];
    char temp_path[96];
    char type[64];

    if(!temp_c) {
        return -1;
    }

    for(int i = 0; i < 16; i++) {
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/thermal/thermal_zone%d/type", i);
        if(ui_read_file_first_line(type_path, type, sizeof(type)) != 0 ||
           !sensor_thermal_type_is_k230(type)) {
            continue;
        }

        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/thermal/thermal_zone%d/temp", i);
        if(sensor_read_cpu_temp_path(temp_path, temp_c) == 0) {
            return 0;
        }
    }

    for(int i = 0; i < 16; i++) {
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/thermal/thermal_zone%d/temp", i);
        if(sensor_read_cpu_temp_path(temp_path, temp_c) == 0) {
            return 0;
        }
    }

    return -1;
}

static int sensor_read_hwmon(sensor_reading_t *reading)
{
    char name_path[96];
    char name[64];
    char temp_path[96];
    char hum_path[96];
    double temp;
    double hum;

    for(int i = 0; i < 12; i++) {
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/hwmon%d/name",
                 i);
        if(ui_read_file_first_line(name_path, name, sizeof(name)) != 0) {
            continue;
        }
        if(strcmp(name, "aht10") != 0 && strcmp(name, "aht20") != 0) {
            continue;
        }
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/hwmon/hwmon%d/temp1_input", i);
        snprintf(hum_path, sizeof(hum_path),
                 "/sys/class/hwmon/hwmon%d/humidity1_input", i);
        if(read_double_from_file(temp_path, 1000.0, &temp) == 0 &&
           read_double_from_file(hum_path, 1000.0, &hum) == 0) {
            reading->ok = 1;
            reading->bus = -1;
            reading->temp_c = temp;
            reading->humidity_pct = hum;
            snprintf(reading->source, sizeof(reading->source),
                     "hwmon%d %s", i, name);
            snprintf(reading->status, sizeof(reading->status), "Ready");
            return 0;
        }
    }
    return -1;
}

static int sensor_i2c_select_addr(int fd)
{
    if(ioctl(fd, I2C_SLAVE, AHT20_ADDR) == 0) {
        return 0;
    }
    return ioctl(fd, I2C_SLAVE_FORCE, AHT20_ADDR);
}

static int sensor_aht20_prepare(int fd)
{
    unsigned char status = 0;
    unsigned char init_cmd[3] = {0xBE, 0x08, 0x00};

    if(read(fd, &status, 1) == 1 && (status & AHT20_STATUS_CALIBRATED)) {
        return 0;
    }

    if(write(fd, init_cmd, sizeof(init_cmd)) != (ssize_t)sizeof(init_cmd)) {
        return -1;
    }
    usleep(10000);
    return 0;
}

static int sensor_read_aht20_bus(int bus, sensor_reading_t *reading)
{
    char dev_path[32];
    int fd;
    unsigned char measure_cmd[3] = {0xAC, 0x33, 0x00};
    unsigned char data[7];
    uint32_t raw_hum;
    uint32_t raw_temp;
    ssize_t got;

    snprintf(dev_path, sizeof(dev_path), "/dev/i2c-%d", bus);
    fd = open(dev_path, O_RDWR);
    if(fd < 0) {
        return -1;
    }

    if(sensor_i2c_select_addr(fd) < 0) {
        close(fd);
        return -1;
    }

    if(sensor_aht20_prepare(fd) != 0) {
        close(fd);
        return -1;
    }

    if(write(fd, measure_cmd, sizeof(measure_cmd)) != (ssize_t)sizeof(measure_cmd)) {
        close(fd);
        return -1;
    }
    usleep(90000);
    got = read(fd, data, sizeof(data));
    if(got >= 1 && (data[0] & AHT20_STATUS_BUSY)) {
        usleep(20000);
        got = read(fd, data, sizeof(data));
    }
    if(got < 6) {
        close(fd);
        return -1;
    }
    close(fd);

    if(data[0] & AHT20_STATUS_BUSY) {
        return -1;
    }

    raw_hum = ((uint32_t)data[1] << 12) |
              ((uint32_t)data[2] << 4) |
              ((uint32_t)data[3] >> 4);
    raw_temp = (((uint32_t)data[3] & 0x0FU) << 16) |
               ((uint32_t)data[4] << 8) |
               (uint32_t)data[5];

    reading->ok = 1;
    reading->bus = bus;
    reading->humidity_pct = (double)raw_hum * 100.0 / 1048576.0;
    reading->temp_c = (double)raw_temp * 200.0 / 1048576.0 - 50.0;
    snprintf(reading->source, sizeof(reading->source), "/dev/i2c-%d 0x38",
             bus);
    snprintf(reading->status, sizeof(reading->status), "Ready");
    return 0;
}

static void sensor_aht20_gpio_delay(void)
{
    usleep(8);
}

static uint32_t sensor_aht20_iomux_value(unsigned int sel)
{
    return (sel << 11) | (1U << 8) | (1U << 7) | (1U << 6) | (8U << 1) | 1U;
}

static int sensor_aht20_iomux_set(unsigned int sel)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        return -1;
    }

    map = mmap(NULL, AHT20_IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               AHT20_IOMUX_BASE);
    close(fd);
    if(map == MAP_FAILED) {
        return -1;
    }

    regs = (volatile uint32_t *)map;
    regs[AHT20_IOMUX_IO46_OFFSET / 4U] = sensor_aht20_iomux_value(sel);
    regs[AHT20_IOMUX_IO47_OFFSET / 4U] = sensor_aht20_iomux_value(sel);
    usleep(1000);
    munmap(map, AHT20_IOMUX_SIZE);
    return 0;
}

typedef struct {
    struct gpiod_line_request *request;
} sensor_gpio_i2c_t;

static int sensor_gpio_i2c_request(sensor_gpio_i2c_t *bus)
{
    struct gpiod_chip *chip = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    unsigned int offsets[2] = {
        AHT20_GPIO_SCL_OFFSET,
        AHT20_GPIO_SDA_OFFSET
    };
    enum gpiod_line_value values[2] = {
        GPIOD_LINE_VALUE_ACTIVE,
        GPIOD_LINE_VALUE_ACTIVE
    };
    int ok = -1;

    if(!bus) {
        return -1;
    }
    memset(bus, 0, sizeof(*bus));

    chip = gpiod_chip_open(AHT20_GPIO_CHIP);
    if(!chip) {
        goto out;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if(!settings || !line_config || !request_config) {
        goto out;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_OPEN_DRAIN);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);
    gpiod_request_config_set_consumer(request_config,
                                      "k230-phone-i2c4-bitbang");

    if(gpiod_line_config_add_line_settings(line_config, offsets, 2,
                                           settings) != 0) {
        goto out;
    }
    if(gpiod_line_config_set_output_values(line_config, values, 2) != 0) {
        goto out;
    }

    bus->request = gpiod_chip_request_lines(chip, request_config, line_config);
    ok = bus->request ? 0 : -1;

out:
    gpiod_request_config_free(request_config);
    gpiod_line_config_free(line_config);
    gpiod_line_settings_free(settings);
    if(chip) {
        gpiod_chip_close(chip);
    }
    return ok;
}

static void sensor_gpio_i2c_release(sensor_gpio_i2c_t *bus)
{
    if(bus && bus->request) {
        gpiod_line_request_release(bus->request);
        bus->request = NULL;
    }
}

static int sensor_gpio_i2c_set(sensor_gpio_i2c_t *bus, unsigned int offset,
                               int high)
{
    return gpiod_line_request_set_value(
        bus->request, offset,
        high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

static int sensor_gpio_i2c_get(sensor_gpio_i2c_t *bus, unsigned int offset)
{
    enum gpiod_line_value value;

    value = gpiod_line_request_get_value(bus->request, offset);
    if(value == GPIOD_LINE_VALUE_ERROR) {
        return -1;
    }
    return value == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0;
}

static void sensor_gpio_i2c_scl(sensor_gpio_i2c_t *bus, int high)
{
    sensor_gpio_i2c_set(bus, AHT20_GPIO_SCL_OFFSET, high);
    sensor_aht20_gpio_delay();
}

static void sensor_gpio_i2c_sda(sensor_gpio_i2c_t *bus, int high)
{
    sensor_gpio_i2c_set(bus, AHT20_GPIO_SDA_OFFSET, high);
    sensor_aht20_gpio_delay();
}

static int sensor_gpio_i2c_read_sda(sensor_gpio_i2c_t *bus)
{
    sensor_aht20_gpio_delay();
    return sensor_gpio_i2c_get(bus, AHT20_GPIO_SDA_OFFSET);
}

static void sensor_gpio_i2c_start(sensor_gpio_i2c_t *bus)
{
    sensor_gpio_i2c_sda(bus, 1);
    sensor_gpio_i2c_scl(bus, 1);
    sensor_gpio_i2c_sda(bus, 0);
    sensor_gpio_i2c_scl(bus, 0);
}

static void sensor_gpio_i2c_stop(sensor_gpio_i2c_t *bus)
{
    sensor_gpio_i2c_sda(bus, 0);
    sensor_gpio_i2c_scl(bus, 1);
    sensor_gpio_i2c_sda(bus, 1);
}

static int sensor_gpio_i2c_write_byte(sensor_gpio_i2c_t *bus, uint8_t value)
{
    int ack;

    for(int bit = 7; bit >= 0; bit--) {
        sensor_gpio_i2c_sda(bus, (value >> bit) & 1U);
        sensor_gpio_i2c_scl(bus, 1);
        sensor_gpio_i2c_scl(bus, 0);
    }

    sensor_gpio_i2c_sda(bus, 1);
    sensor_gpio_i2c_scl(bus, 1);
    ack = sensor_gpio_i2c_read_sda(bus) == 0;
    sensor_gpio_i2c_scl(bus, 0);
    return ack ? 0 : -1;
}

static uint8_t sensor_gpio_i2c_read_byte(sensor_gpio_i2c_t *bus, int ack)
{
    uint8_t value = 0;

    sensor_gpio_i2c_sda(bus, 1);
    for(int bit = 7; bit >= 0; bit--) {
        sensor_gpio_i2c_scl(bus, 1);
        if(sensor_gpio_i2c_read_sda(bus) > 0) {
            value |= (uint8_t)(1U << bit);
        }
        sensor_gpio_i2c_scl(bus, 0);
    }

    sensor_gpio_i2c_sda(bus, ack ? 0 : 1);
    sensor_gpio_i2c_scl(bus, 1);
    sensor_gpio_i2c_scl(bus, 0);
    sensor_gpio_i2c_sda(bus, 1);
    return value;
}

static int keyboard_i2c_begin(sensor_gpio_i2c_t *bus)
{
    if(sensor_aht20_iomux_set(0) != 0) {
        return -1;
    }
    if(sensor_gpio_i2c_request(bus) != 0) {
        sensor_aht20_iomux_set(3);
        return -1;
    }
    return 0;
}

static void keyboard_i2c_end(sensor_gpio_i2c_t *bus)
{
    sensor_gpio_i2c_release(bus);
    sensor_aht20_iomux_set(3);
}

static int keyboard_i2c_probe_addr_unlocked(sensor_gpio_i2c_t *bus,
                                            uint8_t addr)
{
    int ret;

    sensor_gpio_i2c_start(bus);
    ret = sensor_gpio_i2c_write_byte(bus, (uint8_t)(addr << 1));
    sensor_gpio_i2c_stop(bus);
    return ret == 0;
}

static int keyboard_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *value)
{
    sensor_gpio_i2c_t bus;
    int ret = -1;

    if(!value) {
        return -1;
    }

    pthread_mutex_lock(&sensor_aht20_lock);
    if(keyboard_i2c_begin(&bus) != 0) {
        goto out_unlock;
    }

    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)(addr << 1)) != 0) {
        goto out_stop;
    }
    if(sensor_gpio_i2c_write_byte(&bus, reg) != 0) {
        goto out_stop;
    }
    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)((addr << 1) | 1U)) != 0) {
        goto out_stop;
    }
    *value = sensor_gpio_i2c_read_byte(&bus, 0);
    ret = 0;

out_stop:
    sensor_gpio_i2c_stop(&bus);
    keyboard_i2c_end(&bus);
out_unlock:
    pthread_mutex_unlock(&sensor_aht20_lock);
    return ret;
}

static int keyboard_i2c_read_block(uint8_t addr, uint8_t reg, uint8_t *buf,
                                   size_t len)
{
    sensor_gpio_i2c_t bus;
    int ret = -1;

    if(!buf || len == 0) {
        return -1;
    }

    pthread_mutex_lock(&sensor_aht20_lock);
    if(keyboard_i2c_begin(&bus) != 0) {
        goto out_unlock;
    }

    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)(addr << 1)) != 0) {
        goto out_stop;
    }
    if(sensor_gpio_i2c_write_byte(&bus, reg) != 0) {
        goto out_stop;
    }
    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)((addr << 1) | 1U)) != 0) {
        goto out_stop;
    }
    for(size_t i = 0; i < len; i++) {
        buf[i] = sensor_gpio_i2c_read_byte(&bus, i + 1U < len);
    }
    ret = 0;

out_stop:
    sensor_gpio_i2c_stop(&bus);
    keyboard_i2c_end(&bus);
out_unlock:
    pthread_mutex_unlock(&sensor_aht20_lock);
    return ret;
}

static int keyboard_i2c_read_word_le(uint8_t addr, uint8_t reg,
                                     uint16_t *value)
{
    uint8_t buf[2];

    if(!value || keyboard_i2c_read_block(addr, reg, buf, sizeof(buf)) != 0) {
        return -1;
    }
    *value = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return 0;
}

static int keyboard_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    sensor_gpio_i2c_t bus;
    int ret = -1;

    pthread_mutex_lock(&sensor_aht20_lock);
    if(keyboard_i2c_begin(&bus) != 0) {
        goto out_unlock;
    }

    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)(addr << 1)) != 0) {
        goto out_stop;
    }
    if(sensor_gpio_i2c_write_byte(&bus, reg) != 0) {
        goto out_stop;
    }
    if(sensor_gpio_i2c_write_byte(&bus, value) != 0) {
        goto out_stop;
    }
    ret = 0;

out_stop:
    sensor_gpio_i2c_stop(&bus);
    keyboard_i2c_end(&bus);
out_unlock:
    pthread_mutex_unlock(&sensor_aht20_lock);
    return ret;
}

static int keyboard_i2c_update_bits(uint8_t addr, uint8_t reg, uint8_t mask,
                                    uint8_t value)
{
    uint8_t old_value;
    uint8_t new_value;

    if(keyboard_i2c_read_reg(addr, reg, &old_value) != 0) {
        return -1;
    }
    new_value = (uint8_t)((old_value & ~mask) | (value & mask));
    if(new_value == old_value) {
        return 0;
    }
    return keyboard_i2c_write_reg(addr, reg, new_value);
}

static void keyboard_base_probe(void)
{
    static const uint8_t addrs[] = {
        BQ25896_ADDR,
        BQ27220_ADDR,
        TCA8418_ADDR,
        XL9555_ADDR
    };
    int present[4] = {0, 0, 0, 0};
    sensor_gpio_i2c_t bus;

    pthread_mutex_lock(&sensor_aht20_lock);
    if(keyboard_i2c_begin(&bus) == 0) {
        for(size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
            present[i] = keyboard_i2c_probe_addr_unlocked(&bus, addrs[i]);
        }
        keyboard_i2c_end(&bus);
    }
    pthread_mutex_unlock(&sensor_aht20_lock);

    pthread_mutex_lock(&keyboard_base_lock);
    keyboard_base_state.scanned = 1;
    keyboard_base_state.bq25896 = present[0];
    keyboard_base_state.bq27220 = present[1];
    keyboard_base_state.tca8418 = present[2];
    keyboard_base_state.xl9555 = present[3];
    if(present[0] || present[1] || present[2] || present[3]) {
        snprintf(keyboard_base_state.status, sizeof(keyboard_base_state.status),
                 "Detected 6B:%s 55:%s %02X:%s 20:%s",
                 present[0] ? "yes" : "no", present[1] ? "yes" : "no",
                 TCA8418_ADDR, present[2] ? "yes" : "no",
                 present[3] ? "yes" : "no");
    } else {
        snprintf(keyboard_base_state.status, sizeof(keyboard_base_state.status),
                 "Keyboard base not detected");
    }
    pthread_mutex_unlock(&keyboard_base_lock);

    fprintf(stderr, "[keyboard-base] %s on %s\n", keyboard_base_state.status,
            KEYBOARD_BASE_I2C_LABEL);
}

static void keyboard_base_get_state(keyboard_base_state_t *state)
{
    if(!state) {
        return;
    }
    pthread_mutex_lock(&keyboard_base_lock);
    *state = keyboard_base_state;
    pthread_mutex_unlock(&keyboard_base_lock);
}

void ui_hardware_reboot_diag_dump(const char *tag)
{
    keyboard_base_state_t base;
    uint8_t reg_bq25896 = 0;
    uint8_t reg_bq27220 = 0;
    uint8_t reg_tca8418 = 0;
    uint8_t reg_xl9555 = 0;
    double uptime = 0.0;
    time_t now;
    FILE *fp;
    FILE *up;
    int rc_bq25896;
    int rc_bq27220;
    int rc_tca8418;
    int rc_xl9555;

    keyboard_base_probe();
    keyboard_base_get_state(&base);

    rc_bq25896 = keyboard_i2c_read_reg(BQ25896_ADDR,
                                       BQ25896_REG_DEVICE_REV, &reg_bq25896);
    rc_bq27220 = keyboard_i2c_read_reg(BQ27220_ADDR,
                                       BQ27220_REG_VOLTAGE, &reg_bq27220);
    rc_tca8418 = keyboard_i2c_read_reg(TCA8418_ADDR, TCA8418_REG_CFG,
                                       &reg_tca8418);
    rc_xl9555 = keyboard_i2c_read_reg(XL9555_ADDR, XL9555_REG_INPUT0,
                                      &reg_xl9555);

    up = fopen("/proc/uptime", "r");
    if(up) {
        if(fscanf(up, "%lf", &uptime) != 1) {
            uptime = 0.0;
        }
        fclose(up);
    }

    fp = fopen(REBOOT_DIAG_LOG, "a");
    if(!fp) {
        return;
    }

    now = time(NULL);
    fprintf(fp,
            "%llu unix=%lld uptime=%.2f tag=%s scanned=%d "
            "present{bq25896=%d bq27220=%d tca8418=%d xl9555=%d} "
            "read_rc{bq25896=%d bq27220=%d tca8418=%d xl9555=%d} "
            "read_val{bq25896=0x%02X bq27220=0x%02X tca8418=0x%02X xl9555=0x%02X} "
            "runtime{hw_thread=%d ext_req=%d ext_active=%d tca_ready=%d} "
            "status=\"%s\"\n",
            (unsigned long long)ui_monotonic_us(), (long long)now, uptime,
            tag ? tag : "unknown", base.scanned, base.bq25896,
            base.bq27220, base.tca8418, base.xl9555, rc_bq25896,
            rc_bq27220, rc_tca8418, rc_xl9555, reg_bq25896, reg_bq27220,
            reg_tca8418, reg_xl9555, hardware_thread_started,
            extension_keyboard_requested, extension_keyboard_active,
            keyboard_tca8418_ready, base.status);
    fclose(fp);
}

static const char *bq25896_charge_state_name(uint8_t status_reg)
{
    switch((status_reg >> 3) & 0x03) {
    case 0:
        return "Not charging";
    case 1:
        return "Pre-charge";
    case 2:
        return "Fast charge";
    case 3:
        return "Done";
    default:
        return "Unknown";
    }
}

static const char *bq25896_bus_state_name(uint8_t status_reg)
{
    switch((status_reg >> 5) & 0x07) {
    case 0:
        return "No input";
    case 1:
        return "USB SDP";
    case 2:
        return "Adapter";
    case 7:
        return "OTG";
    default:
        return "VBUS";
    }
}

static int bq25896_read(bq25896_reading_t *reading)
{
    uint8_t reg0 = 0;
    uint8_t reg2 = 0;
    uint8_t reg3 = 0;
    uint8_t reg4 = 0;
    uint8_t reg6 = 0;
    uint8_t reg0b = 0;
    uint8_t reg0c = 0;
    uint8_t reg0e = 0;
    uint8_t reg0f = 0;
    uint8_t reg10 = 0;
    uint8_t reg11 = 0;
    uint8_t reg12 = 0;
    uint8_t reg13 = 0;
    uint8_t reg14 = 0;
    int vreg_index;

    if(!reading) {
        return -1;
    }
    memset(reading, 0, sizeof(*reading));
    snprintf(reading->status, sizeof(reading->status), "BQ25896 not detected");

    if(keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_DEVICE_REV, &reg14) != 0) {
        return -1;
    }
    reading->present = 1;
    reading->revision = reg14 & 0x03;
    reading->part_number = (reg14 >> 3) & 0x07;

    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_INPUT, &reg0);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_PWR_ONOFF, &reg2);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_CHG_CTRL, &reg3);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_CHG_CURRENT, &reg4);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_CHG_VOLT, &reg6);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_STATUS, &reg0b);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_FAULT, &reg0c);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_IDPM, &reg13);

    keyboard_i2c_update_bits(BQ25896_ADDR, BQ25896_REG_PWR_ONOFF,
                             BQ25896_MASK_CONV_START,
                             BQ25896_MASK_CONV_START);
    usleep(90000);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_ADC_BATV, &reg0e);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_ADC_SYSV, &reg0f);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_ADC_NTC, &reg10);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_ADC_BUSV, &reg11);
    keyboard_i2c_read_reg(BQ25896_ADDR, BQ25896_REG_ADC_ICHGR, &reg12);

    (void)reg2;
    reading->charge_enabled = (reg3 & BQ25896_MASK_CHG_CONFIG) ? 1 : 0;
    reading->fast_charge_ma = (reg4 & BQ25896_MASK_ICHG) *
                              BQ25896_FAST_CHG_STEP_MA;
    if(reading->fast_charge_ma > BQ25896_FAST_CHG_MAX_MA) {
        reading->fast_charge_ma = BQ25896_FAST_CHG_MAX_MA;
    }
    reading->input_limit_ma = BQ25896_INPUT_CURRENT_BASE_MA +
                              ((reg0 & 0x3F) *
                               BQ25896_INPUT_CURRENT_STEP_MA);
    vreg_index = (reg6 & BQ25896_MASK_VREG) >> 2;
    reading->charge_voltage_mv = BQ25896_CHG_VOLT_BASE_MV +
                                 vreg_index * BQ25896_CHG_VOLT_STEP_MV;
    if(reading->charge_voltage_mv > 4608) {
        reading->charge_voltage_mv = 4608;
    }
    reading->vbat_mv = (reg0e & 0x7F) ?
                       (BQ25896_ADC_VOLT_BASE_MV +
                        (reg0e & 0x7F) * BQ25896_ADC_VBAT_STEP_MV) : 0;
    reading->vsys_mv = BQ25896_ADC_VOLT_BASE_MV +
                       (reg0f & 0x7F) * BQ25896_ADC_VSYS_STEP_MV;
    reading->vbus_mv = (reg11 & 0x80) ?
                       (BQ25896_ADC_VBUS_BASE_MV +
                        (reg11 & 0x7F) * BQ25896_ADC_VBUS_STEP_MV) : 0;
    reading->charge_current_ma = (reg12 & 0x7F) *
                                 BQ25896_ADC_ICHG_STEP_MA;
    reading->ntc_pct = BQ25896_ADC_NTC_BASE_PCT +
                       (double)(reg10 & 0x7F) * BQ25896_ADC_NTC_STEP_PCT;
    reading->status_reg = reg0b;
    reading->fault_reg = reg0c;
    reading->idpm_reg = reg13;
    reading->ok = 1;
    snprintf(reading->status, sizeof(reading->status), "Ready");
    return 0;
}

static int bq25896_set_fast_charge_current(int ma)
{
    int closest;
    uint8_t value;

    if(ma <= BQ25896_FAST_CHG_STEP_MA / 2) {
        closest = 0;
    } else {
        closest = ((ma + BQ25896_FAST_CHG_STEP_MA / 2) /
                   BQ25896_FAST_CHG_STEP_MA) *
                  BQ25896_FAST_CHG_STEP_MA;
    }
    closest = clamp_int(closest, 0, BQ25896_FAST_CHG_MAX_MA);
    value = (uint8_t)(closest / BQ25896_FAST_CHG_STEP_MA);
    write_pref_int(PREF_BQ25896_ICHG_MA, closest);
    return keyboard_i2c_update_bits(BQ25896_ADDR, BQ25896_REG_CHG_CURRENT,
                                    BQ25896_MASK_ICHG, value);
}

static int bq25896_set_charge_enabled(int enabled)
{
    return keyboard_i2c_update_bits(BQ25896_ADDR, BQ25896_REG_CHG_CTRL,
                                    BQ25896_MASK_CHG_CONFIG,
                                    enabled ? BQ25896_MASK_CHG_CONFIG : 0);
}

static void bq25896_apply_startup_pref(void)
{
    int ma = read_pref_int(PREF_BQ25896_ICHG_MA, 512, 0,
                           BQ25896_FAST_CHG_MAX_MA);
    int rc = bq25896_set_fast_charge_current(ma);

    button_test_log("BQ25896 startup charge current pref=%dmA rc=%d", ma, rc);
}

static const char *bq27220_flow_state(const bq27220_reading_t *reading)
{
    if(!reading) {
        return "Unknown";
    }
    if(reading->current_ma > 0) {
        return "Charging";
    }
    if(reading->current_ma < 0) {
        return "Discharging";
    }
    return "Idle";
}

static int bq27220_read_word(uint8_t reg, uint16_t *value)
{
    return keyboard_i2c_read_word_le(BQ27220_ADDR, reg, value);
}

static int bq27220_read(bq27220_reading_t *reading)
{
    uint16_t temp = 0;
    uint16_t voltage = 0;
    uint16_t battery_status = 0;
    uint16_t current = 0;
    uint16_t remaining = 0;
    uint16_t full = 0;
    uint16_t tte = 0;
    uint16_t ttf = 0;
    uint16_t standby = 0;
    uint16_t max_load = 0;
    uint16_t avg_power = 0;
    uint16_t internal_temp = 0;
    uint16_t cycle = 0;
    uint16_t soc = 0;
    uint16_t soh = 0;
    uint16_t chg_voltage = 0;
    uint16_t chg_current = 0;
    uint16_t op_status = 0;
    uint16_t design = 0;

    if(!reading) {
        return -1;
    }
    memset(reading, 0, sizeof(*reading));
    snprintf(reading->status, sizeof(reading->status), "BQ27220 not detected");

    if(bq27220_read_word(BQ27220_REG_VOLTAGE, &voltage) != 0) {
        return -1;
    }
    if(bq27220_read_word(BQ27220_REG_SOC, &soc) != 0 ||
       voltage <= 2500U || voltage >= 6000U || soc > 100U) {
        return -1;
    }
    reading->present = 1;

    bq27220_read_word(BQ27220_REG_TEMP, &temp);
    bq27220_read_word(BQ27220_REG_BATTERY_STATUS, &battery_status);
    bq27220_read_word(BQ27220_REG_CURRENT, &current);
    bq27220_read_word(BQ27220_REG_REMAINING_CAPACITY, &remaining);
    bq27220_read_word(BQ27220_REG_FULL_CHARGE_CAPACITY, &full);
    bq27220_read_word(BQ27220_REG_TIME_TO_EMPTY, &tte);
    bq27220_read_word(BQ27220_REG_TIME_TO_FULL, &ttf);
    bq27220_read_word(BQ27220_REG_STANDBY_CURRENT, &standby);
    bq27220_read_word(BQ27220_REG_MAX_LOAD_CURRENT, &max_load);
    bq27220_read_word(BQ27220_REG_AVERAGE_POWER, &avg_power);
    bq27220_read_word(BQ27220_REG_INTERNAL_TEMP, &internal_temp);
    bq27220_read_word(BQ27220_REG_CYCLE_COUNT, &cycle);
    bq27220_read_word(BQ27220_REG_SOH, &soh);
    bq27220_read_word(BQ27220_REG_CHARGING_VOLTAGE, &chg_voltage);
    bq27220_read_word(BQ27220_REG_CHARGING_CURRENT, &chg_current);
    bq27220_read_word(BQ27220_REG_OPERATION_STATUS, &op_status);
    bq27220_read_word(BQ27220_REG_DESIGN_CAPACITY, &design);

    reading->voltage_mv = voltage;
    reading->temp_c = temp ? ((double)temp * 0.1 - 273.15) : 0.0;
    reading->internal_temp_c = internal_temp ?
                               ((double)internal_temp * 0.1 - 273.15) : 0.0;
    reading->battery_status = battery_status;
    reading->current_ma = (int16_t)current;
    reading->remaining_mah = remaining;
    reading->full_charge_mah = full;
    reading->time_to_empty_min = tte;
    reading->time_to_full_min = ttf;
    reading->standby_current_ma = (int16_t)standby;
    reading->max_load_current_ma = (int16_t)max_load;
    reading->average_power_mw = (int16_t)avg_power;
    reading->cycle_count = cycle;
    reading->soc_pct = soc;
    reading->soh_pct = soh;
    reading->charging_voltage_mv = chg_voltage;
    reading->charging_current_ma = chg_current;
    reading->operation_status = op_status;
    reading->design_mah = design;
    reading->ok = 1;
    snprintf(reading->status, sizeof(reading->status), "Ready");
    return 0;
}

int ui_hardware_get_cpu_temp_c(double *temp_c)
{
    if(!temp_c) {
        return -1;
    }
    return sensor_read_cpu_temp(temp_c);
}

int ui_hardware_get_aht20(double *temp_c, double *humidity_pct)
{
    sensor_reading_t reading;

    if(sensor_read_aht20(&reading) != 0) {
        return -1;
    }
    if(temp_c) {
        *temp_c = reading.temp_c;
    }
    if(humidity_pct) {
        *humidity_pct = reading.humidity_pct;
    }
    return 0;
}

int ui_bq25896_get_usb_present(int *present, int *vbus_mv)
{
    if(present) {
        *present = 0;
    }
    if(vbus_mv) {
        *vbus_mv = 0;
    }

    return ui_bq25896_get_power_state(present, vbus_mv, NULL);
}

int ui_bq25896_get_power_state(int *usb_present, int *vbus_mv, int *vbat_mv)
{
    bq25896_reading_t reading;
    int vbus_state;

    if(usb_present) {
        *usb_present = 0;
    }
    if(vbus_mv) {
        *vbus_mv = 0;
    }
    if(vbat_mv) {
        *vbat_mv = 0;
    }
    if(bq25896_read(&reading) != 0) {
        return -1;
    }

    vbus_state = (reading.status_reg >> 5) & 0x07;
    if(usb_present) {
        *usb_present = (vbus_state != 0 && vbus_state != 7) ||
                       reading.vbus_mv > 3900;
    }
    if(vbus_mv) {
        *vbus_mv = reading.vbus_mv;
    }
    if(vbat_mv) {
        *vbat_mv = reading.vbat_mv;
    }
    return 0;
}

int ui_bq25896_get_charge_state(int *charging, int *done)
{
    bq25896_reading_t reading;
    int state;

    if(charging) {
        *charging = 0;
    }
    if(done) {
        *done = 0;
    }
    if(bq25896_read(&reading) != 0) {
        return -1;
    }

    state = (reading.status_reg >> 3) & 0x03;
    if(charging) {
        *charging = (state == 1 || state == 2);
    }
    if(done) {
        *done = (state == 3);
    }
    return 0;
}

int ui_bq27220_get_current_ma(int *current_ma)
{
    bq27220_reading_t reading;

    if(!current_ma || bq27220_read(&reading) != 0) {
        return -1;
    }
    *current_ma = reading.current_ma;
    return 0;
}

int ui_bq27220_get_voltage_mv(int *voltage_mv)
{
    bq27220_reading_t reading;

    if(!voltage_mv || bq27220_read(&reading) != 0) {
        return -1;
    }
    *voltage_mv = reading.voltage_mv;
    return 0;
}

int ui_bq27220_get_soc_pct(int *soc_pct)
{
    bq27220_reading_t reading;

    if(!soc_pct || bq27220_read(&reading) != 0) {
        return -1;
    }
    *soc_pct = clamp_int(reading.soc_pct, 0, 100);
    return 0;
}

static const char *tca8418_key_name(int code)
{
    static const char *const names[TCA8418_ROWS * TCA8418_COLS + 1] = {
        [1] = "RIGHT",
        [2] = "LEFT",
        [3] = "FN-R",
        [5] = "SPACE",
        [6] = "TAB",
        [7] = "SHIFT",
        [8] = "LILYGO",
        [9] = "FN",
        [10] = "CAPS",
        [11] = "MIC",
        [12] = "DOWN",
        [13] = "M",
        [14] = "SPACE",
        [15] = "V",
        [16] = "C",
        [17] = "X",
        [18] = "Z",
        [19] = "ALT",
        [20] = "Q",
        [21] = "ENTER",
        [22] = "UP",
        [23] = "CTRL",
        [24] = "N",
        [25] = "B",
        [26] = "F",
        [27] = "D",
        [28] = "S",
        [29] = "A",
        [32] = "L",
        [33] = "K",
        [34] = "J",
        [35] = "H",
        [36] = "G",
        [37] = "R",
        [38] = "E",
        [39] = "W",
        [40] = "ESC",
        [41] = "DEL",
        [42] = "P",
        [43] = "O",
        [44] = "I",
        [45] = "U",
        [46] = "Y",
        [47] = "T",
        [48] = "2",
        [49] = "1",
        [50] = "F1",
        [51] = "0",
        [52] = "9",
        [53] = "8",
        [54] = "7",
        [55] = "6",
        [56] = "5",
        [57] = "4",
        [58] = "3",
        [59] = "F3",
        [60] = "F2",
        [61] = "F11",
        [62] = "F10",
        [63] = "F9",
        [64] = "F8",
        [65] = "F7",
        [66] = "F6",
        [67] = "F5",
        [68] = "F4",
    };

    if(code < 0 || code > TCA8418_ROWS * TCA8418_COLS || !names[code]) {
        return NULL;
    }
    return names[code];
}

static const char *tca8418_key_grid_label(int code)
{
    switch(code) {
    case 1:
        return "RGT";
    case 2:
        return "LFT";
    case 3:
        return "FN-R";
    case 5:
    case 14:
        return "SPC";
    case 7:
        return "SH";
    case 8:
        return "LOGO";
    case 10:
        return "CAP";
    case 12:
        return "DN";
    case 21:
        return "ENT";
    case 23:
        return "CTL";
    default:
        break;
    }
    return tca8418_key_name(code);
}

static void tca8418_key_grid_text(int code, int compact, char *out,
                                  size_t out_len)
{
    const char *primary = tca8418_key_grid_label(code);
    uint32_t shifted = extension_keyboard_shift_symbol_for_code(code);
    char primary_buf[8];
    char shift_buf[8];

    if(!out || out_len == 0) {
        return;
    }
    out[0] = '\0';

    if(!primary) {
        snprintf(primary_buf, sizeof(primary_buf), "%02d", code);
        primary = primary_buf;
    }
    if(!shifted) {
        snprintf(out, out_len, "%s", primary);
        return;
    }

    snprintf(shift_buf, sizeof(shift_buf), "%c", (char)shifted);
    snprintf(out, out_len, compact ? "%s/%s" : "%s\n%s", primary, shift_buf);
}

static const int keyboard_display_layout[KEYBOARD_LAYOUT_ROWS]
                                        [KEYBOARD_LAYOUT_COLS] = {
    { 50, 60, 59, 68, 67, 66, 65, 64, 63, 62, 61 },
    { 40, 49, 48, 58, 57, 56, 55, 54, 53, 52, 51 },
    { 20, 39, 38, 37, 47, 46, 45, 44, 43, 42, 41 },
    { 10, 29, 28, 27, 26, 36, 35, 34, 33, 32, 21 },
    { 19, 18, 17, 16, 15, 25, 24, 13, 23, 22, 11 },
    { 9, 8, 7, 6, 5, 14, 3, 2, 12, 1, 0 },
};

static int keyboard_code_is_pressed(int code)
{
    int row;
    int col;
    int pressed;

    if(code < 1 || code > TCA8418_ROWS * TCA8418_COLS) {
        return 0;
    }
    row = (code - 1) / TCA8418_COLS;
    col = (code - 1) % TCA8418_COLS;
    pthread_mutex_lock(&tca8418_event_lock);
    pressed = keyboard_key_pressed[row][col];
    pthread_mutex_unlock(&tca8418_event_lock);
    return pressed;
}

static int extension_keyboard_fn_pressed(void)
{
    return keyboard_code_is_pressed(9) || keyboard_code_is_pressed(3);
}

static int extension_keyboard_esc_back_enabled(void)
{
    return read_pref_int(PREF_EXTENSION_KEYBOARD_ESC_BACK, 1, 0, 1);
}

static void extension_keyboard_set_esc_back_enabled(int enabled)
{
    write_pref_int(PREF_EXTENSION_KEYBOARD_ESC_BACK, enabled ? 1 : 0);
}

static void extension_keyboard_toggle_backlight(void)
{
    int current = keyboard_backlight_current_or_pref();
    int target = 0;

    if(current <= 0) {
        target = read_pref_int(PREF_KEYBOARD_BACKLIGHT,
                               KEYBOARD_BACKLIGHT_DEFAULT_PCT, 1, 100);
    }
    keyboard_backlight_apply(target, keyboard_backlight_pref_frequency_hz(), 0);
    fprintf(stderr, "[extension-keyboard] FN+B keyboard backlight=%d%%\n",
            target);
}

static int keyboard_hotkey_code_to_index(int code)
{
    for(size_t i = 0; i < sizeof(keyboard_hotkey_fkeys) /
           sizeof(keyboard_hotkey_fkeys[0]); i++) {
        if(keyboard_hotkey_fkeys[i].code == code) {
            return (int)i;
        }
    }
    return -1;
}

static const char *keyboard_hotkey_pref_key(int f_index, char *buf,
                                            size_t len)
{
    if(!buf || len == 0) {
        return "";
    }
    snprintf(buf, len, "%s%d", PREF_EXTENSION_KEYBOARD_HOTKEY_PREFIX,
             f_index + 1);
    return buf;
}

static keyboard_hotkey_action_t keyboard_hotkey_normalize_action(int value)
{
    if(value < 0 || value >= KEYBOARD_HOTKEY_COUNT) {
        return KEYBOARD_HOTKEY_NONE;
    }
    return (keyboard_hotkey_action_t)value;
}

static keyboard_hotkey_action_t keyboard_hotkey_get_action(int f_index)
{
    char key[48];
    keyboard_hotkey_action_t fallback;

    if(f_index < 0 || f_index >= KEYBOARD_HOTKEY_FKEY_COUNT) {
        return KEYBOARD_HOTKEY_NONE;
    }
    fallback = keyboard_hotkey_fkeys[f_index].fallback;
    return keyboard_hotkey_normalize_action(
        read_pref_int(keyboard_hotkey_pref_key(f_index, key, sizeof(key)),
                      (int)fallback, 0, KEYBOARD_HOTKEY_COUNT - 1));
}

static void keyboard_hotkey_set_action(int f_index,
                                       keyboard_hotkey_action_t action)
{
    char key[48];

    if(f_index < 0 || f_index >= KEYBOARD_HOTKEY_FKEY_COUNT) {
        return;
    }
    write_pref_int(keyboard_hotkey_pref_key(f_index, key, sizeof(key)),
                   (int)keyboard_hotkey_normalize_action((int)action));
}

static const char *keyboard_hotkey_action_label(keyboard_hotkey_action_t action)
{
    action = keyboard_hotkey_normalize_action((int)action);
    for(size_t i = 0; i < sizeof(keyboard_hotkey_actions) /
           sizeof(keyboard_hotkey_actions[0]); i++) {
        if(keyboard_hotkey_actions[i].action == action) {
            return keyboard_hotkey_actions[i].label;
        }
    }
    return "None";
}

static keyboard_hotkey_action_t keyboard_hotkey_next_action(
    keyboard_hotkey_action_t action)
{
    int next = (int)keyboard_hotkey_normalize_action((int)action) + 1;

    if(next >= KEYBOARD_HOTKEY_COUNT) {
        next = 0;
    }
    return (keyboard_hotkey_action_t)next;
}

static int keyboard_hotkey_next_rotation(int degrees)
{
    switch(degrees) {
    case 0:
        return 90;
    case 90:
        return 180;
    case 180:
        return 270;
    default:
        return 0;
    }
}

static void keyboard_hotkey_adjust_volume(int delta)
{
    int value = ui_audio_get_volume_value();
    int max_value = ui_audio_get_volume_max();
    int step;

    if(max_value <= 0) {
        max_value = 100;
    }
    step = max_value / 12;
    if(step < 1) {
        step = 1;
    }
    ui_audio_set_volume_value(clamp_int(value + delta * step, 0, max_value),
                              1);
}

static int keyboard_hotkey_run_action(int f_index,
                                      keyboard_hotkey_action_t action)
{
    action = keyboard_hotkey_normalize_action((int)action);
    if(action == KEYBOARD_HOTKEY_NONE) {
        return 0;
    }

    fprintf(stderr, "[extension-keyboard] hotkey %s action=%s\n",
            f_index >= 0 && f_index < KEYBOARD_HOTKEY_FKEY_COUNT ?
            keyboard_hotkey_fkeys[f_index].name : "F?",
            keyboard_hotkey_action_label(action));

    switch(action) {
    case KEYBOARD_HOTKEY_HOME:
        app_nav_to_page(PAGE_HOME);
        return 1;
    case KEYBOARD_HOTKEY_SETTINGS:
        app_nav_to_page(PAGE_SETTINGS);
        return 1;
    case KEYBOARD_HOTKEY_TERMINAL:
        app_nav_to_page(PAGE_TERMINAL);
        return 1;
    case KEYBOARD_HOTKEY_MESHTASTIC:
        app_nav_to_page(PAGE_MESHTASTIC);
        return 1;
    case KEYBOARD_HOTKEY_CAMERA:
        app_nav_to_page(PAGE_CAMERA);
        return 1;
    case KEYBOARD_HOTKEY_SCREENSHOT:
        app_take_screenshot();
        return 1;
    case KEYBOARD_HOTKEY_ROTATE_NEXT:
        app_set_display_rotation_degrees(
            keyboard_hotkey_next_rotation(app_display_rotation_degrees()));
        return 1;
    case KEYBOARD_HOTKEY_ROTATE_0:
        app_set_display_rotation_degrees(0);
        return 1;
    case KEYBOARD_HOTKEY_ROTATE_90:
        app_set_display_rotation_degrees(90);
        return 1;
    case KEYBOARD_HOTKEY_ROTATE_180:
        app_set_display_rotation_degrees(180);
        return 1;
    case KEYBOARD_HOTKEY_ROTATE_270:
        app_set_display_rotation_degrees(270);
        return 1;
    case KEYBOARD_HOTKEY_VOLUME_DOWN:
        keyboard_hotkey_adjust_volume(-1);
        return 1;
    case KEYBOARD_HOTKEY_VOLUME_UP:
        keyboard_hotkey_adjust_volume(1);
        return 1;
    case KEYBOARD_HOTKEY_KEYBOARD_BACKLIGHT:
        extension_keyboard_toggle_backlight();
        return 1;
    case KEYBOARD_HOTKEY_NONE:
    case KEYBOARD_HOTKEY_COUNT:
    default:
        break;
    }

    return 0;
}

static int keyboard_hotkey_handle_fkey(int code)
{
    int f_index = keyboard_hotkey_code_to_index(code);

    if(f_index < 0) {
        return 0;
    }
    return keyboard_hotkey_run_action(f_index,
                                      keyboard_hotkey_get_action(f_index));
}

static void extension_keyboard_set_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(extension_keyboard_status, sizeof(extension_keyboard_status),
              fmt, ap);
    va_end(ap);
}

static int tca8418_raw_queue_full_locked(void)
{
    return ((tca8418_raw_head + 1) % TCA8418_RAW_QUEUE_SIZE) ==
           tca8418_raw_tail;
}

static int tca8418_raw_queue_empty_locked(void)
{
    return tca8418_raw_head == tca8418_raw_tail;
}

static void tca8418_raw_queue_clear(void)
{
    pthread_mutex_lock(&tca8418_event_lock);
    tca8418_raw_head = 0;
    tca8418_raw_tail = 0;
    pthread_mutex_unlock(&tca8418_event_lock);
}

static void tca8418_raw_queue_push_locked(int code, int pressed)
{
    if(code <= 0) {
        return;
    }
    if(tca8418_raw_queue_full_locked()) {
        tca8418_raw_tail =
            (tca8418_raw_tail + 1) % TCA8418_RAW_QUEUE_SIZE;
    }
    tca8418_raw_queue[tca8418_raw_head].code = code;
    tca8418_raw_queue[tca8418_raw_head].pressed = pressed;
    tca8418_raw_head = (tca8418_raw_head + 1) % TCA8418_RAW_QUEUE_SIZE;
}

static int tca8418_raw_queue_pop(tca8418_raw_event_t *event)
{
    if(!event) {
        return 0;
    }

    pthread_mutex_lock(&tca8418_event_lock);
    if(tca8418_raw_queue_empty_locked()) {
        pthread_mutex_unlock(&tca8418_event_lock);
        return 0;
    }
    *event = tca8418_raw_queue[tca8418_raw_tail];
    tca8418_raw_tail = (tca8418_raw_tail + 1) % TCA8418_RAW_QUEUE_SIZE;
    pthread_mutex_unlock(&tca8418_event_lock);
    return 1;
}

static void extension_keyboard_queue_clear(void)
{
    extension_key_head = 0;
    extension_key_tail = 0;
}

static int extension_keyboard_queue_full(void)
{
    return ((extension_key_head + 1) % EXT_KEY_QUEUE_SIZE) == extension_key_tail;
}

static int extension_keyboard_queue_empty(void)
{
    return extension_key_head == extension_key_tail;
}

static void extension_keyboard_queue_push(uint32_t key, lv_indev_state_t state)
{
    if(key == 0) {
        return;
    }
    if(extension_keyboard_queue_full()) {
        extension_key_tail = (extension_key_tail + 1) % EXT_KEY_QUEUE_SIZE;
    }
    extension_key_queue[extension_key_head].key = key;
    extension_key_queue[extension_key_head].state = state;
    extension_key_head = (extension_key_head + 1) % EXT_KEY_QUEUE_SIZE;
}

static int extension_keyboard_queue_pop(extension_key_event_t *event)
{
    if(extension_keyboard_queue_empty() || !event) {
        return 0;
    }
    *event = extension_key_queue[extension_key_tail];
    extension_key_tail = (extension_key_tail + 1) % EXT_KEY_QUEUE_SIZE;
    return 1;
}

static void extension_keyboard_deliver_key(int code, uint32_t key)
{
    if(key == 0) {
        return;
    }
    if(extension_keyboard_key_cb) {
        extension_keyboard_key_cb(code, key, 1,
                                  extension_keyboard_key_user_data);
        return;
    }
    extension_keyboard_queue_push(key, LV_INDEV_STATE_PRESSED);
    extension_keyboard_queue_push(key, LV_INDEV_STATE_RELEASED);
}

static int extension_keyboard_is_modifier_code(int code)
{
    return code == 7 || code == 9 || code == 10 || code == 19 ||
           code == 23 || code == 3 || code == 8 || code == 11;
}

static void extension_keyboard_repeat_clear(void)
{
    extension_keyboard_repeat_code = 0;
    extension_keyboard_repeat_key = 0;
    extension_keyboard_repeat_next_us = 0;
}

static void extension_keyboard_repeat_start(int code, uint32_t key)
{
    if(key == 0 || extension_keyboard_is_modifier_code(code)) {
        extension_keyboard_repeat_clear();
        return;
    }
    extension_keyboard_repeat_code = code;
    extension_keyboard_repeat_key = key;
    extension_keyboard_repeat_next_us = ui_monotonic_us() +
                                        EXT_KEY_REPEAT_START_US;
}

static void extension_keyboard_repeat_poll(void)
{
    uint64_t now;
    int emitted = 0;

    if(!extension_keyboard_repeat_code || extension_keyboard_repeat_key == 0) {
        return;
    }
    if(!keyboard_code_is_pressed(extension_keyboard_repeat_code)) {
        extension_keyboard_repeat_clear();
        return;
    }

    now = ui_monotonic_us();
    while(extension_keyboard_repeat_next_us &&
          now >= extension_keyboard_repeat_next_us && emitted < 4) {
        extension_keyboard_deliver_key(extension_keyboard_repeat_code,
                                       extension_keyboard_repeat_key);
        extension_keyboard_repeat_next_us += EXT_KEY_REPEAT_PERIOD_US;
        emitted++;
    }
    if(emitted > 0) {
        fprintf(stderr, "[extension-keyboard] repeat code=%d count=%d\n",
                extension_keyboard_repeat_code, emitted);
    }
}

static uint32_t extension_keyboard_shift_symbol_for_code(int code)
{
    switch(code) {
    case 49:
        return '!';
    case 48:
        return '@';
    case 58:
        return '#';
    case 57:
        return '$';
    case 56:
        return '%';
    case 55:
        return '^';
    case 54:
        return '&';
    case 53:
        return '*';
    case 52:
        return '(';
    case 51:
        return ')';
    case 20:
        return '`';
    case 39:
        return '~';
    case 38:
        return '-';
    case 37:
        return '+';
    case 47:
        return '=';
    case 46:
        return '\\';
    case 45:
        return '|';
    case 44:
        return ';';
    case 43:
        return ':';
    case 42:
        return '"';
    case 29:
        return '~';
    case 28:
        return '[';
    case 27:
        return ']';
    case 26:
        return '{';
    case 36:
        return '}';
    case 35:
        return ',';
    case 34:
        return '`';
    case 33:
        return '/';
    case 32:
        return '?';
    case 25:
        return '.';
    case 24:
        return '<';
    case 13:
        return '>';
    default:
        return 0;
    }
}

static uint32_t extension_keyboard_ascii_for_code(int code)
{
    int shift = keyboard_code_is_pressed(7);
    int upper = extension_keyboard_caps ^ shift;
    uint32_t shifted;

    if(shift) {
        shifted = extension_keyboard_shift_symbol_for_code(code);
        if(shifted) {
            return shifted;
        }
    }

    switch(code) {
    case 49:
        return '1';
    case 48:
        return '2';
    case 58:
        return '3';
    case 57:
        return '4';
    case 56:
        return '5';
    case 55:
        return '6';
    case 54:
        return '7';
    case 53:
        return '8';
    case 52:
        return '9';
    case 51:
        return '0';
    default:
        break;
    }

    const char *name = tca8418_key_name(code);
    if(name && name[0] >= 'A' && name[0] <= 'Z' && name[1] == '\0') {
        return (uint32_t)(upper ? name[0] : (name[0] - 'A' + 'a'));
    }
    return 0;
}

static uint32_t extension_keyboard_lv_key_for_code(int code)
{
    switch(code) {
    case 1:
        return LV_KEY_RIGHT;
    case 2:
        return LV_KEY_LEFT;
    case 5:
    case 14:
        return ' ';
    case 6:
        return LV_KEY_NEXT;
    case 12:
        return LV_KEY_DOWN;
    case 21:
        return LV_KEY_ENTER;
    case 22:
        return LV_KEY_UP;
    case 40:
        return LV_KEY_ESC;
    case 41:
        return LV_KEY_BACKSPACE;
    default:
        break;
    }
    return extension_keyboard_ascii_for_code(code);
}

static void extension_keyboard_enqueue_tca_event(int code, int pressed)
{
    uint32_t key;

    if(!extension_keyboard_active) {
        return;
    }
    app_note_user_activity();

    if(!pressed) {
        if(code == extension_keyboard_repeat_code) {
            extension_keyboard_repeat_clear();
        }
        if(extension_keyboard_key_cb) {
            key = extension_keyboard_lv_key_for_code(code);
            extension_keyboard_key_cb(code, key, 0,
                                      extension_keyboard_key_user_data);
        }
        return;
    }

    if(keyboard_hotkey_handle_fkey(code)) {
        extension_keyboard_repeat_clear();
        return;
    }

    if(code == 10) {
        extension_keyboard_caps = !extension_keyboard_caps;
        xl9555_set_led(0, extension_keyboard_caps);
        fprintf(stderr, "[extension-keyboard] caps=%d\n",
                extension_keyboard_caps);
        extension_keyboard_repeat_clear();
        return;
    }
    if(code == 8 && extension_keyboard_fn_pressed()) {
        fprintf(stderr, "[extension-keyboard] FN+LILYGO screenshot\n");
        app_take_screenshot();
        extension_keyboard_repeat_clear();
        return;
    }
    if((code == 5 || code == 14) && extension_keyboard_fn_pressed()) {
        ui_extension_keyboard_toggle_pinyin();
        if(extension_keyboard_key_cb) {
            extension_keyboard_key_cb(code, 0, pressed,
                                      extension_keyboard_key_user_data);
        }
        extension_keyboard_repeat_clear();
        return;
    }
    if(extension_keyboard_is_modifier_code(code)) {
        if(extension_keyboard_key_cb) {
            extension_keyboard_key_cb(code, 0, pressed,
                                      extension_keyboard_key_user_data);
        }
        extension_keyboard_repeat_clear();
        return;
    }
    if(code == 25 && extension_keyboard_fn_pressed()) {
        extension_keyboard_toggle_backlight();
        extension_keyboard_repeat_clear();
        return;
    }
    if(code == 40 && extension_keyboard_esc_back_enabled() &&
       !extension_keyboard_key_cb) {
        app_nav_back();
        extension_keyboard_repeat_clear();
        return;
    }

    key = extension_keyboard_lv_key_for_code(code);
    if(key == 0) {
        extension_keyboard_repeat_clear();
        return;
    }
    extension_keyboard_deliver_key(code, key);
    extension_keyboard_repeat_start(code, key);
}

static void tca8418_reset_pin(void)
{
    gpio_set_value_once(TCA8418_RST_GPIO, 0, "k230-phone-tca8418-rst");
    usleep(3000);
    gpio_set_value_once(TCA8418_RST_GPIO, 1, "k230-phone-tca8418-rst");
    usleep(12000);
}

static int tca8418_flush_events(void)
{
    uint8_t event = 0;
    int flushed = 0;

    for(int i = 0; i < 16; i++) {
        if(keyboard_i2c_read_reg(TCA8418_ADDR, TCA8418_REG_KEY_EVENT_A,
                                 &event) != 0) {
            return -1;
        }
        if(event == 0) {
            break;
        }
        flushed++;
    }
    keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_INT_STAT,
                           TCA8418_STAT_K_INT | TCA8418_STAT_OVR_FLOW_INT);
    return flushed;
}

static int tca8418_init_device(void)
{
    uint8_t key_count = 0;

    tca8418_prepare_irq_gpio();
    tca8418_reset_pin();
    if(keyboard_i2c_read_reg(TCA8418_ADDR, TCA8418_REG_KEY_LCK_EC,
                             &key_count) != 0) {
        keyboard_tca8418_ready = 0;
        return -1;
    }

    if(keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_KP_GPIO_1,
                              0x7F) != 0 ||
       keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_KP_GPIO_2,
                              0xFF) != 0 ||
       keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_KP_GPIO_3,
                              0x03) != 0) {
        keyboard_tca8418_ready = 0;
        return -1;
    }

    keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_DEBOUNCE_DIS_1, 0x00);
    keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_DEBOUNCE_DIS_2, 0x00);
    keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_DEBOUNCE_DIS_3, 0x00);
    keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_CFG,
                           TCA8418_CFG_MODE);
    tca8418_flush_events();
    keyboard_tca8418_ready = 1;
    return 0;
}

static int keyboard_write_click_raw(void)
{
    FILE *fp = fopen(KEYBOARD_CLICK_RAW_PATH, "wb");

    if(!fp) {
        return -1;
    }

    for(int i = 0; i < 640; i++) {
        int16_t sample;

        if(i < 64) {
            sample = 12000;
        } else if(i < 128) {
            sample = -9000;
        } else if(i < 224) {
            sample = 4800;
        } else if(i < 320) {
            sample = -2400;
        } else {
            sample = 0;
        }

        if(fwrite(&sample, sizeof(sample), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    keyboard_click_raw_ready = 1;
    return 0;
}

static void keyboard_play_key_sound(void)
{
    uint64_t now;
    int rc;

    if(!keyboard_test_sound_enabled) {
        return;
    }

    now = ui_monotonic_us();
    if(keyboard_last_click_us &&
       now - keyboard_last_click_us < 45000ULL) {
        return;
    }
    keyboard_last_click_us = now;

    if(!keyboard_click_raw_ready && keyboard_write_click_raw() != 0) {
        return;
    }

    rc = system("aplay -q -D default -t raw -f S16_LE -c 1 -r "
                "48000 " KEYBOARD_CLICK_RAW_PATH
                " >/dev/null 2>>" KEYBOARD_CLICK_LOG_PATH " &");
    (void)rc;
}

static int tca8418_read_fifo_events(int *event_count)
{
    uint8_t int_stat = 0;
    uint8_t count_reg = 0;
    uint8_t event = 0;
    int count;
    int handled = 0;
    int int_stat_valid = 0;
    int overflow = 0;

    if(event_count) {
        *event_count = 0;
    }
    if(!keyboard_tca8418_ready) {
        uint64_t now = ui_monotonic_us();
        if(keyboard_last_init_us &&
           now - keyboard_last_init_us < 2000000ULL) {
            return -1;
        }
        keyboard_last_init_us = now;
        if(tca8418_init_device() != 0) {
            return -1;
        }
    }

    if(keyboard_i2c_read_reg(TCA8418_ADDR, TCA8418_REG_INT_STAT,
                             &int_stat) == 0) {
        int_stat_valid = 1;
        overflow = (int_stat & TCA8418_STAT_OVR_FLOW_INT) != 0;
    }
    if(keyboard_i2c_read_reg(TCA8418_ADDR, TCA8418_REG_KEY_LCK_EC,
                             &count_reg) != 0) {
        keyboard_tca8418_ready = 0;
        return -1;
    }
    count = count_reg & 0x0F;
    if(event_count) {
        *event_count = count;
    }
    pthread_mutex_lock(&tca8418_event_lock);
    tca8418_last_fifo_count = count;
    if(overflow) {
        memset(keyboard_key_pressed, 0, sizeof(keyboard_key_pressed));
        tca8418_raw_head = 0;
        tca8418_raw_tail = 0;
        tca8418_reset_dispatch_requested = 1;
        tca8418_overflow_count++;
        snprintf(keyboard_last_event_text, sizeof(keyboard_last_event_text),
                 "TCA8418 overflow recovered int=0x%02X fifo=%d",
                 int_stat, count);
    }
    pthread_mutex_unlock(&tca8418_event_lock);
    if(overflow) {
        extension_keyboard_repeat_clear();
        fprintf(stderr, "[tca8418] overflow recovered int=0x%02X fifo=%d\n",
                int_stat, count);
    }

    for(int i = 0; i < 32; i++) {
        int pressed;
        int code;
        int row;
        int col;

        if(i >= count && count > 0) {
            break;
        }
        if(keyboard_i2c_read_reg(TCA8418_ADDR, TCA8418_REG_KEY_EVENT_A,
                                 &event) != 0) {
            keyboard_tca8418_ready = 0;
            return handled > 0 ? handled : -1;
        }
        if(event == 0) {
            break;
        }
        pressed = (event & 0x80) ? 1 : 0;
        code = event & 0x7F;
        if(code >= 1 && code <= TCA8418_ROWS * TCA8418_COLS) {
            const char *name = tca8418_key_name(code);

            row = (code - 1) / TCA8418_COLS;
            col = (code - 1) % TCA8418_COLS;
            pthread_mutex_lock(&tca8418_event_lock);
            keyboard_key_pressed[row][col] = pressed;
            keyboard_event_total++;
            tca8418_raw_queue_push_locked(code, pressed);
            snprintf(keyboard_last_event_text, sizeof(keyboard_last_event_text),
                     "%s raw=0x%02X code=%02d row=%d col=%d %s",
                     name ? name : "UNMAPPED", event, code, row, col,
                     pressed ? "press" : "release");
            pthread_mutex_unlock(&tca8418_event_lock);
            if(pressed) {
                keyboard_play_key_sound();
            }
        } else {
            pthread_mutex_lock(&tca8418_event_lock);
            snprintf(keyboard_last_event_text, sizeof(keyboard_last_event_text),
                     "raw=0x%02X code=%02d GPIO/unknown", event, code);
            pthread_mutex_unlock(&tca8418_event_lock);
        }
        handled++;
    }

    if(handled > 0 ||
       (int_stat_valid &&
        (int_stat & (TCA8418_STAT_K_INT | TCA8418_STAT_K_LCK_INT |
                     TCA8418_STAT_OVR_FLOW_INT)))) {
        keyboard_i2c_write_reg(TCA8418_ADDR, TCA8418_REG_INT_STAT,
                               TCA8418_STAT_K_INT | TCA8418_STAT_K_LCK_INT |
                               TCA8418_STAT_OVR_FLOW_INT);
    }
    return handled;
}

static void extension_keyboard_process_raw_events(void)
{
    tca8418_raw_event_t event;
    int processed = 0;
    int reset_dispatch = 0;

    pthread_mutex_lock(&tca8418_event_lock);
    if(tca8418_reset_dispatch_requested) {
        tca8418_reset_dispatch_requested = 0;
        reset_dispatch = 1;
    }
    pthread_mutex_unlock(&tca8418_event_lock);

    if(reset_dispatch) {
        extension_keyboard_repeat_clear();
        extension_keyboard_queue_clear();
    }

    while(processed < TCA8418_RAW_QUEUE_SIZE &&
          tca8418_raw_queue_pop(&event)) {
        extension_keyboard_enqueue_tca_event(event.code, event.pressed);
        processed++;
    }
}

static int tca8418_irq_open_request(void)
{
#if K230_TCA8418_USE_IRQ
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    unsigned int chip_index = TCA8418_IRQ_GPIO / 32U;
    unsigned int offset = TCA8418_IRQ_GPIO % 32U;
    char chip_path[32];
    int rc = -1;

    if(tca8418_irq_request) {
        return 0;
    }
    if(chip_index > 1U) {
        extension_keyboard_set_status("TCA8418 IRQ GPIO%u out of range",
                                      TCA8418_IRQ_GPIO);
        return -1;
    }

    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%u", chip_index);
    tca8418_irq_chip = gpiod_chip_open(chip_path);
    if(!tca8418_irq_chip) {
        extension_keyboard_set_status("TCA8418 IRQ open %s failed: %s",
                                      chip_path, strerror(errno));
        return -1;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if(!settings || !line_config || !request_config) {
        extension_keyboard_set_status("TCA8418 IRQ alloc failed");
        goto out;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_edge_detection(settings,
                                           GPIOD_LINE_EDGE_FALLING);
    gpiod_request_config_set_consumer(request_config,
                                      "k230-phone-tca8418-irq");
    gpiod_request_config_set_event_buffer_size(request_config, 16);

    if(gpiod_line_config_add_line_settings(line_config, &offset, 1,
                                           settings) != 0) {
        extension_keyboard_set_status("TCA8418 IRQ config GPIO%u failed: %s",
                                      TCA8418_IRQ_GPIO, strerror(errno));
        goto out;
    }

    tca8418_irq_request =
        gpiod_chip_request_lines(tca8418_irq_chip, request_config,
                                 line_config);
    if(!tca8418_irq_request) {
        extension_keyboard_set_status("TCA8418 IRQ request GPIO%u failed: %s",
                                      TCA8418_IRQ_GPIO, strerror(errno));
        goto out;
    }
    rc = 0;

out:
    gpiod_request_config_free(request_config);
    gpiod_line_config_free(line_config);
    gpiod_line_settings_free(settings);
    if(rc != 0) {
        if(tca8418_irq_request) {
            gpiod_line_request_release(tca8418_irq_request);
            tca8418_irq_request = NULL;
        }
        if(tca8418_irq_chip) {
            gpiod_chip_close(tca8418_irq_chip);
            tca8418_irq_chip = NULL;
        }
    }
    return rc;
#else
    return -1;
#endif
}

static void tca8418_irq_close_request(void)
{
    if(tca8418_irq_request) {
        gpiod_line_request_release(tca8418_irq_request);
        tca8418_irq_request = NULL;
    }
    if(tca8418_irq_chip) {
        gpiod_chip_close(tca8418_irq_chip);
        tca8418_irq_chip = NULL;
    }
    pthread_mutex_lock(&tca8418_event_lock);
    tca8418_irq_ready = 0;
    pthread_mutex_unlock(&tca8418_event_lock);
}

static int tca8418_irq_line_pending(void)
{
#if K230_TCA8418_USE_IRQ
    enum gpiod_line_value value;
    unsigned int offset = TCA8418_IRQ_GPIO % 32U;

    if(!tca8418_irq_request) {
        return 0;
    }
    value = gpiod_line_request_get_value(tca8418_irq_request, offset);
    if(value == GPIOD_LINE_VALUE_ERROR) {
        pthread_mutex_lock(&tca8418_event_lock);
        tca8418_irq_error_count++;
        pthread_mutex_unlock(&tca8418_event_lock);
        return 0;
    }
    return value == GPIOD_LINE_VALUE_INACTIVE;
#else
    return 0;
#endif
}

static void *tca8418_irq_thread_entry(void *arg)
{
    struct gpiod_edge_event_buffer *buffer;

    (void)arg;

    buffer = gpiod_edge_event_buffer_new(16);
    if(!buffer) {
        pthread_mutex_lock(&tca8418_event_lock);
        tca8418_irq_error_count++;
        pthread_mutex_unlock(&tca8418_event_lock);
        return NULL;
    }

    pthread_mutex_lock(&tca8418_event_lock);
    tca8418_irq_ready = 1;
    pthread_mutex_unlock(&tca8418_event_lock);

    tca8418_read_fifo_events(NULL);
    while(!tca8418_irq_stop) {
        int wait_rc;
        int read_rc;

        wait_rc = gpiod_line_request_wait_edge_events(tca8418_irq_request,
                                                      50000000LL);
        if(tca8418_irq_stop) {
            break;
        }
        if(wait_rc == 0) {
            if(tca8418_irq_line_pending()) {
                pthread_mutex_lock(&tca8418_event_lock);
                tca8418_irq_safety_count++;
                pthread_mutex_unlock(&tca8418_event_lock);
                for(int i = 0; i < 4; i++) {
                    int handled = tca8418_read_fifo_events(NULL);

                    if(handled <= 0) {
                        break;
                    }
                }
            }
            continue;
        }
        if(wait_rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            pthread_mutex_lock(&tca8418_event_lock);
            tca8418_irq_error_count++;
            pthread_mutex_unlock(&tca8418_event_lock);
            usleep(50000);
            continue;
        }

        read_rc = gpiod_line_request_read_edge_events(tca8418_irq_request,
                                                      buffer, 16);
        if(read_rc < 0) {
            pthread_mutex_lock(&tca8418_event_lock);
            tca8418_irq_error_count++;
            pthread_mutex_unlock(&tca8418_event_lock);
            continue;
        }

        pthread_mutex_lock(&tca8418_event_lock);
        tca8418_irq_count += read_rc > 0 ? (unsigned int)read_rc : 1U;
        tca8418_irq_last_us = ui_monotonic_us();
        pthread_mutex_unlock(&tca8418_event_lock);

        for(int i = 0; i < 4; i++) {
            int handled = tca8418_read_fifo_events(NULL);

            if(handled <= 0) {
                break;
            }
        }
    }

    gpiod_edge_event_buffer_free(buffer);
    pthread_mutex_lock(&tca8418_event_lock);
    tca8418_irq_ready = 0;
    pthread_mutex_unlock(&tca8418_event_lock);
    return NULL;
}

static int tca8418_irq_start(void)
{
#if K230_TCA8418_USE_IRQ
    if(tca8418_irq_thread_started) {
        return 0;
    }
    if(!keyboard_tca8418_ready && tca8418_init_device() != 0) {
        return -1;
    }
    if(tca8418_irq_open_request() != 0) {
        return -1;
    }

    tca8418_raw_queue_clear();
    tca8418_irq_stop = 0;
    if(pthread_create(&tca8418_irq_thread, NULL,
                      tca8418_irq_thread_entry, NULL) != 0) {
        tca8418_irq_close_request();
        extension_keyboard_set_status("TCA8418 IRQ thread start failed");
        return -1;
    }
    tca8418_irq_thread_started = 1;
    return 0;
#else
    return 0;
#endif
}

static void tca8418_irq_stop_thread(void)
{
    if(!tca8418_irq_thread_started) {
        return;
    }

    tca8418_irq_stop = 1;
    pthread_join(tca8418_irq_thread, NULL);
    tca8418_irq_thread_started = 0;
    tca8418_irq_stop = 0;
    tca8418_irq_close_request();
}

static void extension_keyboard_disable_runtime(const char *reason, int save_pref)
{
    extension_keyboard_requested = 0;
    extension_keyboard_active = 0;
    extension_keyboard_fail_count = 0;
    tca8418_irq_stop_thread();
    keyboard_tca8418_ready = 0;
    extension_keyboard_caps = 0;
    extension_keyboard_pinyin = 0;
    extension_keyboard_repeat_clear();
    extension_keyboard_queue_clear();
    tca8418_raw_queue_clear();
    xl9555_set_led(0, 0);
    xl9555_set_led(1, 0);
    ui_input_set_soft_keyboard_enabled(1);
    if(save_pref) {
        ui_prefs_set(PREF_EXTENSION_KEYBOARD, "0");
        lv_async_call(extension_keyboard_refresh_async, NULL);
    }
    extension_keyboard_set_status("%s", reason ? reason :
                                  "Extension keyboard off");
    fprintf(stderr, "[extension-keyboard] disabled: %s\n",
            extension_keyboard_status);
}

static int extension_keyboard_enable_runtime(int save_pref)
{
    keyboard_base_state_t base;

    keyboard_base_probe();
    keyboard_base_get_state(&base);
    if(!base.tca8418) {
        extension_keyboard_disable_runtime("TCA8418 not detected", save_pref);
        return -1;
    }

    keyboard_tca8418_ready = 0;
    keyboard_last_init_us = 0;
    if(tca8418_init_device() != 0) {
        extension_keyboard_disable_runtime("TCA8418 init failed", save_pref);
        return -1;
    }
    if(tca8418_irq_start() != 0) {
        extension_keyboard_disable_runtime("TCA8418 IRQ start failed",
                                           save_pref);
        return -1;
    }

    extension_keyboard_requested = 1;
    extension_keyboard_active = 1;
    extension_keyboard_fail_count = 0;
    extension_keyboard_caps = 0;
    extension_keyboard_pinyin = 0;
    extension_keyboard_repeat_clear();
    extension_keyboard_queue_clear();
    tca8418_raw_queue_clear();
    xl9555_set_led(0, 0);
    xl9555_set_led(1, 0);
    ui_input_set_soft_keyboard_enabled(0);
    if(save_pref) {
        ui_prefs_set(PREF_EXTENSION_KEYBOARD, "1");
    }
    if(ui_extension_keyboard_auto_rotate_enabled() &&
       app_display_rotation_degrees() != 270) {
        ui_prefs_set("display.rotation", "270");
        lv_async_call(extension_keyboard_rotate_270_async, NULL);
    }
    extension_keyboard_set_status("TCA8418 IRQ active at 0x%02X GPIO%u",
                                  TCA8418_ADDR, TCA8418_IRQ_GPIO);
    fprintf(stderr, "[extension-keyboard] enabled: %s\n",
            extension_keyboard_status);
    return 0;
}

static void extension_keyboard_read_cb(lv_indev_t *indev,
                                       lv_indev_data_t *data)
{
    extension_key_event_t event;

    (void)indev;
    if(!data) {
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;
    data->continue_reading = false;

    if(!extension_keyboard_requested || !extension_keyboard_active) {
        return;
    }

    extension_keyboard_process_raw_events();
    extension_keyboard_fail_count = 0;
    extension_keyboard_repeat_poll();

    if(extension_keyboard_queue_pop(&event)) {
        data->key = event.key;
        data->state = event.state;
        data->continue_reading = !extension_keyboard_queue_empty();
    }
}

static int extension_keyboard_interval_valid(int seconds)
{
    return seconds == 3 || seconds == 10 || seconds == 30;
}

static int extension_keyboard_normalize_interval(int seconds)
{
    if(seconds <= 3) {
        return 3;
    }
    if(seconds <= 10) {
        return 10;
    }
    return 30;
}

int ui_extension_keyboard_auto_detect_enabled(void)
{
    return read_pref_int(PREF_EXTENSION_KEYBOARD_AUTO_DETECT,
                         EXT_KEY_AUTO_DETECT_DEFAULT, 0, 1);
}

int ui_extension_keyboard_auto_detect_interval_s(void)
{
    int seconds = read_pref_int(PREF_EXTENSION_KEYBOARD_AUTO_INTERVAL,
                                EXT_KEY_AUTO_INTERVAL_DEFAULT_S,
                                EXT_KEY_AUTO_INTERVAL_MIN_S,
                                EXT_KEY_AUTO_INTERVAL_MAX_S);

    return extension_keyboard_interval_valid(seconds) ?
           seconds : extension_keyboard_normalize_interval(seconds);
}

int ui_extension_keyboard_auto_rotate_enabled(void)
{
    return read_pref_int(PREF_EXTENSION_KEYBOARD_AUTO_ROTATE, 1, 0, 1);
}

void ui_extension_keyboard_set_auto_rotate_enabled(int enabled)
{
    write_pref_int(PREF_EXTENSION_KEYBOARD_AUTO_ROTATE, enabled ? 1 : 0);
}

int ui_extension_keyboard_probe_now(void)
{
    extension_keyboard_next_auto_probe_us = 0;
    return extension_keyboard_enable_runtime(0);
}

void ui_extension_keyboard_set_auto_detect_interval_s(int seconds)
{
    write_pref_int(PREF_EXTENSION_KEYBOARD_AUTO_INTERVAL,
                   extension_keyboard_normalize_interval(seconds));
    extension_keyboard_next_auto_probe_us = 0;
}

void ui_extension_keyboard_set_auto_detect_enabled(int enabled)
{
    ui_prefs_set(PREF_EXTENSION_KEYBOARD_AUTO_DETECT, enabled ? "1" : "0");
    extension_keyboard_next_auto_probe_us = 0;
    if(enabled) {
        ui_extension_keyboard_probe_now();
    } else {
        ui_prefs_set(PREF_EXTENSION_KEYBOARD, "0");
        extension_keyboard_disable_runtime("Keyboard auto detect off", 0);
    }
    lv_async_call(extension_keyboard_refresh_async, NULL);
}

static void extension_keyboard_auto_detect_poll(void)
{
    uint64_t now;
    int interval_s;

    if(!ui_extension_keyboard_auto_detect_enabled()) {
        return;
    }

    now = ui_monotonic_us();
    interval_s = ui_extension_keyboard_auto_detect_interval_s();
    if(extension_keyboard_next_auto_probe_us &&
       now < extension_keyboard_next_auto_probe_us) {
        return;
    }

    extension_keyboard_next_auto_probe_us =
        now + (uint64_t)interval_s * 1000000ULL;

    if(extension_keyboard_active) {
        return;
    }

    if(extension_keyboard_enable_runtime(0) == 0) {
        ui_prefs_set(PREF_EXTENSION_KEYBOARD, "1");
    }
}

static void extension_keyboard_apply_startup_pref(void)
{
    if(ui_extension_keyboard_auto_detect_enabled()) {
        if(extension_keyboard_enable_runtime(0) != 0) {
            extension_keyboard_set_status("Keyboard auto detect waiting");
            extension_keyboard_next_auto_probe_us =
                ui_monotonic_us() +
                (uint64_t)ui_extension_keyboard_auto_detect_interval_s() *
                1000000ULL;
        }
        return;
    }
    ui_prefs_set(PREF_EXTENSION_KEYBOARD, "0");
    extension_keyboard_disable_runtime("Keyboard auto detect off", 0);
}

void ui_extension_keyboard_register_indev(void)
{
    if(extension_keyboard_indev) {
        return;
    }

    extension_keyboard_group = lv_group_create();
    lv_group_set_default(extension_keyboard_group);

    extension_keyboard_indev = lv_indev_create();
    lv_indev_set_type(extension_keyboard_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(extension_keyboard_indev, extension_keyboard_read_cb);
    lv_indev_set_group(extension_keyboard_indev, extension_keyboard_group);
}

int ui_extension_keyboard_enabled(void)
{
    return extension_keyboard_requested;
}

int ui_extension_keyboard_active(void)
{
    return extension_keyboard_active;
}

int ui_extension_keyboard_set_enabled(int enabled)
{
    if(enabled) {
        return extension_keyboard_enable_runtime(1);
    }
    extension_keyboard_disable_runtime("Extension keyboard off", 1);
    return 0;
}

int ui_extension_keyboard_base_available(void)
{
    return extension_keyboard_active;
}

const char *ui_extension_keyboard_status(void)
{
    return extension_keyboard_status;
}

void ui_extension_keyboard_focus_obj(lv_obj_t *obj)
{
    if(!obj || !extension_keyboard_group || !extension_keyboard_active) {
        return;
    }
    if(lv_obj_get_group(obj) != extension_keyboard_group) {
        lv_group_add_obj(extension_keyboard_group, obj);
    }
    lv_group_focus_obj(obj);
}

void ui_extension_keyboard_set_key_cb(ui_extension_keyboard_key_cb_t cb,
                                      void *user_data)
{
    extension_keyboard_key_cb = cb;
    extension_keyboard_key_user_data = user_data;
}

int ui_extension_keyboard_pinyin_enabled(void)
{
    return extension_keyboard_pinyin;
}

void ui_extension_keyboard_set_pinyin_enabled(int enabled)
{
    int next = enabled ? 1 : 0;
    int rc;

    extension_keyboard_pinyin = next;
    rc = xl9555_set_led(1, next);
    fprintf(stderr, "[extension-keyboard] pinyin=%d led_p04_rc=%d\n",
            next, rc);
}

void ui_extension_keyboard_toggle_pinyin(void)
{
    ui_extension_keyboard_set_pinyin_enabled(!extension_keyboard_pinyin);
}

static int xl9555_read_regs(uint8_t *input0, uint8_t *output0,
                            uint8_t *config0)
{
    uint8_t value = 0;

    if(input0) {
        if(keyboard_i2c_read_reg(XL9555_ADDR, XL9555_REG_INPUT0,
                                 input0) != 0) {
            return -1;
        }
    } else if(keyboard_i2c_read_reg(XL9555_ADDR, XL9555_REG_INPUT0,
                                    &value) != 0) {
        return -1;
    }

    if(output0 &&
       keyboard_i2c_read_reg(XL9555_ADDR, XL9555_REG_OUTPUT0, output0) != 0) {
        return -1;
    }
    if(config0 &&
       keyboard_i2c_read_reg(XL9555_ADDR, XL9555_REG_CONFIG0, config0) != 0) {
        return -1;
    }
    return 0;
}

static int xl9555_init_device(void)
{
    static const uint8_t bits[3] = {
        XL9555_LED_P03_BIT,
        XL9555_LED_P04_BIT,
        XL9555_LED_P05_BIT
    };
    uint8_t output0 = 0;
    uint8_t config0 = 0;

    if(xl9555_read_regs(NULL, &output0, &config0) != 0) {
        xl9555_ready = 0;
        return -1;
    }

    (void)config0;
    if(keyboard_i2c_update_bits(XL9555_ADDR, XL9555_REG_OUTPUT0,
                                XL9555_LED_MASK, XL9555_LED_MASK) != 0) {
        xl9555_ready = 0;
        return -1;
    }
    if(keyboard_i2c_update_bits(XL9555_ADDR, XL9555_REG_CONFIG0,
                                XL9555_LED_MASK, 0) != 0) {
        xl9555_ready = 0;
        return -1;
    }

    if(keyboard_i2c_read_reg(XL9555_ADDR, XL9555_REG_OUTPUT0, &output0) != 0) {
        xl9555_ready = 0;
        return -1;
    }
    for(size_t i = 0; i < 3; i++) {
        xl9555_led_state[i] = (output0 & (uint8_t)(1U << bits[i])) ? 0 : 1;
    }
    xl9555_ready = 1;
    return 0;
}

static int xl9555_set_led(int index, int enabled)
{
    static const uint8_t bits[3] = {
        XL9555_LED_P03_BIT,
        XL9555_LED_P04_BIT,
        XL9555_LED_P05_BIT
    };
    uint8_t mask;

    if(index < 0 || index >= 3) {
        return -1;
    }
    if(!xl9555_ready && xl9555_init_device() != 0) {
        return -1;
    }

    mask = (uint8_t)(1U << bits[index]);
    if(keyboard_i2c_update_bits(XL9555_ADDR, XL9555_REG_OUTPUT0, mask,
                                enabled ? 0 : mask) != 0) {
        xl9555_ready = 0;
        return -1;
    }
    xl9555_led_state[index] = enabled ? 1 : 0;
    return 0;
}

static int xl9555_set_all(int enabled)
{
    if(!xl9555_ready && xl9555_init_device() != 0) {
        return -1;
    }
    if(keyboard_i2c_update_bits(XL9555_ADDR, XL9555_REG_OUTPUT0,
                                XL9555_LED_MASK,
                                enabled ? 0 : XL9555_LED_MASK) != 0) {
        xl9555_ready = 0;
        return -1;
    }
    for(size_t i = 0; i < 3; i++) {
        xl9555_led_state[i] = enabled ? 1 : 0;
    }
    return 0;
}

static int sensor_read_aht20_gpio(sensor_reading_t *reading)
{
    sensor_gpio_i2c_t bus;
    unsigned char data[6];
    uint32_t raw_hum;
    uint32_t raw_temp;
    int ret = -1;

    if(sensor_aht20_iomux_set(0) != 0) {
        return -1;
    }

    if(sensor_gpio_i2c_request(&bus) != 0) {
        sensor_aht20_iomux_set(3);
        return -1;
    }

    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)(AHT20_ADDR << 1)) != 0) {
        goto out_stop;
    }
    sensor_gpio_i2c_write_byte(&bus, 0xBE);
    sensor_gpio_i2c_write_byte(&bus, 0x08);
    sensor_gpio_i2c_write_byte(&bus, 0x00);
    sensor_gpio_i2c_stop(&bus);
    usleep(10000);

    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)(AHT20_ADDR << 1)) != 0) {
        goto out_stop;
    }
    sensor_gpio_i2c_write_byte(&bus, 0xAC);
    sensor_gpio_i2c_write_byte(&bus, 0x33);
    sensor_gpio_i2c_write_byte(&bus, 0x00);
    sensor_gpio_i2c_stop(&bus);
    usleep(90000);

    sensor_gpio_i2c_start(&bus);
    if(sensor_gpio_i2c_write_byte(&bus, (uint8_t)((AHT20_ADDR << 1) | 1U)) != 0) {
        goto out_stop;
    }
    for(size_t i = 0; i < sizeof(data); i++) {
        data[i] = sensor_gpio_i2c_read_byte(&bus, i + 1U < sizeof(data));
    }
    sensor_gpio_i2c_stop(&bus);

    if(data[0] & AHT20_STATUS_BUSY) {
        goto out_release;
    }

    raw_hum = ((uint32_t)data[1] << 12) |
              ((uint32_t)data[2] << 4) |
              ((uint32_t)data[3] >> 4);
    raw_temp = (((uint32_t)data[3] & 0x0FU) << 16) |
               ((uint32_t)data[4] << 8) |
               (uint32_t)data[5];

    reading->ok = 1;
    reading->bus = -2;
    reading->humidity_pct = (double)raw_hum * 100.0 / 1048576.0;
    reading->temp_c = (double)raw_temp * 200.0 / 1048576.0 - 50.0;
    snprintf(reading->source, sizeof(reading->source),
             "GPIO bitbang IO46/IO47 0x38");
    snprintf(reading->status, sizeof(reading->status), "Ready");
    ret = 0;
    goto out_release;

out_stop:
    sensor_gpio_i2c_stop(&bus);
out_release:
    sensor_gpio_i2c_release(&bus);
    sensor_aht20_iomux_set(3);
    return ret;
}

static int sensor_read_aht20(sensor_reading_t *reading)
{
    int ret = -1;

    if(!reading) {
        return -1;
    }
    pthread_mutex_lock(&sensor_aht20_lock);
    memset(reading, 0, sizeof(*reading));
    snprintf(reading->status, sizeof(reading->status), "AHT20 not detected");
    snprintf(reading->source, sizeof(reading->source),
             "/dev/i2c-%d I2C4 IO47/IO46", AHT20_LINUX_I2C_BUS);

    if(sensor_read_hwmon(reading) == 0) {
        ret = 0;
        goto out;
    }

    if(sensor_aht20_force_gpio &&
       sensor_read_aht20_gpio(reading) == 0) {
        ret = 0;
        goto out;
    }

    if(sensor_read_aht20_bus(AHT20_LINUX_I2C_BUS, reading) == 0) {
        ret = 0;
        goto out;
    }

    if(sensor_read_aht20_gpio(reading) == 0) {
        sensor_aht20_force_gpio = 1;
        ret = 0;
    }

out:
    pthread_mutex_unlock(&sensor_aht20_lock);
    return ret;
}

static int sensor_read_control_temp(double *temp_c, char *source,
                                    size_t source_len)
{
    sensor_reading_t aht;

    if(sensor_read_aht20(&aht) == 0) {
        if(temp_c) {
            *temp_c = aht.temp_c;
        }
        if(source && source_len > 0) {
            snprintf(source, source_len, "%s", aht.source);
        }
        return 0;
    }

    if(sensor_read_cpu_temp(temp_c) == 0) {
        if(source && source_len > 0) {
            snprintf(source, source_len, "CPU thermal");
        }
        return 0;
    }

    if(source && source_len > 0) {
        snprintf(source, source_len, "No sensor");
    }
    return -1;
}

static const char *audio_default_output(void)
{
    return AUDIO_OUTPUT_EXTERNAL;
}

static void audio_apply_output(const char *output)
{
    int external = output && strcmp(output, AUDIO_OUTPUT_EXTERNAL) == 0;

    ui_audio_output_set_external(external ? 1 : 0);
}

static void fan_apply_policy_once(void)
{
    char mode[16];
    char source[64];
    double temp = 0.0;
    int duty = read_pref_int(PREF_FAN_DUTY, 60, 0, 100);
    int on_temp = read_pref_int(PREF_FAN_ON_TEMP, 55, 20, 100);
    int off_temp = read_pref_int(PREF_FAN_OFF_TEMP, 45, 10, 95);
    int target = 0;

    if(!K230_FAN_ENABLED) {
        fan_auto_running = 0;
        return;
    }

    if(off_temp >= on_temp) {
        off_temp = on_temp - 5;
    }

    read_pref_text(PREF_FAN_MODE, mode, sizeof(mode), FAN_MODE_OFF);
    if(strcmp(mode, FAN_MODE_ON) == 0) {
        target = duty;
        fan_auto_running = 1;
    } else if(strcmp(mode, FAN_MODE_AUTO) == 0) {
        if(sensor_read_control_temp(&temp, source, sizeof(source)) == 0) {
            if(temp >= (double)on_temp) {
                fan_auto_running = 1;
            } else if(temp <= (double)off_temp) {
                fan_auto_running = 0;
            }
        }
        target = fan_auto_running ? duty : 0;
    } else {
        fan_auto_running = 0;
        target = 0;
    }

    fan_apply_duty(target);
}

static void *hardware_thread_entry(void *arg)
{
    (void)arg;

    while(!hardware_thread_stop) {
        boot0_toggle_poll();
        fan_apply_policy_once();
        extension_keyboard_auto_detect_poll();
        for(int i = 0; i < 20 && !hardware_thread_stop; i++) {
            boot0_toggle_poll();
            extension_keyboard_auto_detect_poll();
            usleep(100000);
        }
    }
    fan_apply_duty(0);
    return NULL;
}

void ui_hardware_startup(void)
{
    char output[24];
    int keyboard_pref;

    keyboard_base_probe();
    button_prepare_boot0_gpio();
    keyboard_pref = read_pref_int(PREF_KEYBOARD_BACKLIGHT,
                                  KEYBOARD_BACKLIGHT_DEFAULT_PCT, 0, 100);
    keyboard_backlight_log("startup pref=%d", keyboard_pref);
    if(keyboard_pref <= 0) {
        (void)gpio_set_value_once(KEYBOARD_BACKLIGHT_GPIO, 0,
                                  "k230-phone-kbd-bl-startup");
        pthread_mutex_lock(&hardware_lock);
        keyboard_backlight_last = 0;
        keyboard_backlight_freq_last = keyboard_backlight_pref_frequency_hz();
        snprintf(keyboard_backlight_status, sizeof(keyboard_backlight_status),
                 "Keyboard backlight off at startup");
        pthread_mutex_unlock(&hardware_lock);
        keyboard_backlight_log("startup pref=0 skip-pwm");
    } else {
        keyboard_backlight_apply(keyboard_pref,
                                 keyboard_backlight_pref_frequency_hz(), 0);
    }
    bq25896_apply_startup_pref();
    extension_keyboard_apply_startup_pref();

    read_pref_text(PREF_AUDIO_OUTPUT, output, sizeof(output),
                   audio_default_output());
    audio_apply_output(output);

    if(!hardware_thread_started) {
        hardware_thread_stop = 0;
        if(pthread_create(&hardware_thread, NULL, hardware_thread_entry,
                          NULL) == 0) {
            hardware_thread_started = 1;
        }
    }
}

void ui_hardware_shutdown(void)
{
    if(hardware_thread_started) {
        hardware_thread_stop = 1;
        pthread_join(hardware_thread, NULL);
        hardware_thread_started = 0;
    }
    amp_gpio_release();
}

static void style_choice_button(lv_obj_t *btn, int selected, uint32_t accent)
{
    uint32_t count;

    if(!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? accent : 0x202832), 0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(selected ? accent : 0x2A3037),
                                  0);
    count = lv_obj_get_child_count(btn);
    for(uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        lv_obj_set_style_text_color(child,
                                    lv_color_hex(selected ? 0xFFFFFF : accent),
                                    0);
    }
}

static const int keyboard_settings_interval_options[] = {3, 10, 30};

static void keyboard_settings_update_hotkey_button(int f_index)
{
    lv_obj_t *btn;
    lv_obj_t *lbl;
    keyboard_hotkey_action_t action;
    char text[96];

    if(f_index < 0 || f_index >= KEYBOARD_HOTKEY_FKEY_COUNT) {
        return;
    }
    btn = keyboard_settings_hotkey_btn[f_index];
    if(!btn || !lv_obj_is_valid(btn)) {
        return;
    }
    action = keyboard_hotkey_get_action(f_index);
    snprintf(text, sizeof(text), "%s  %s",
             keyboard_hotkey_fkeys[f_index].name,
             ui_tr(keyboard_hotkey_action_label(action)));
    lbl = lv_obj_get_child(btn, 0);
    if(lbl && lv_obj_is_valid(lbl)) {
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl,
                                   ui_font_for_text(text,
                                                    &lv_font_montserrat_18),
                                   0);
        lv_obj_set_width(lbl, lv_obj_get_width(btn) - 18);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
    }
    style_choice_button(btn, action != KEYBOARD_HOTKEY_NONE, 0x3DA5FF);
}

static void keyboard_settings_update_ui(void)
{
    char text[192];
    int auto_enabled = ui_extension_keyboard_auto_detect_enabled();
    int interval_s = ui_extension_keyboard_auto_detect_interval_s();

    if(keyboard_settings_auto_switch &&
       lv_obj_is_valid(keyboard_settings_auto_switch)) {
        if(auto_enabled) {
            lv_obj_add_state(keyboard_settings_auto_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(keyboard_settings_auto_switch, LV_STATE_CHECKED);
        }
    }
    if(keyboard_settings_esc_back_switch &&
       lv_obj_is_valid(keyboard_settings_esc_back_switch)) {
        if(extension_keyboard_esc_back_enabled()) {
            lv_obj_add_state(keyboard_settings_esc_back_switch,
                             LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(keyboard_settings_esc_back_switch,
                               LV_STATE_CHECKED);
        }
    }

    for(size_t i = 0; i < sizeof(keyboard_settings_interval_options) /
           sizeof(keyboard_settings_interval_options[0]); i++) {
        style_choice_button(keyboard_settings_interval_btn[i],
                            interval_s == keyboard_settings_interval_options[i],
                            0xF97316);
    }

    if(keyboard_settings_status_label &&
       lv_obj_is_valid(keyboard_settings_status_label)) {
        snprintf(text, sizeof(text), "%s  %ds\n%s",
                 auto_enabled ? ui_tr("Auto detect on") :
                 ui_tr("Auto detect off"),
                 interval_s, ui_tr(ui_extension_keyboard_status()));
        lv_label_set_text(keyboard_settings_status_label, text);
    }

    for(int i = 0; i < KEYBOARD_HOTKEY_FKEY_COUNT; i++) {
        keyboard_settings_update_hotkey_button(i);
    }
}

static void keyboard_settings_esc_back_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);

    extension_keyboard_set_esc_back_enabled(
        sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
    keyboard_settings_update_ui();
    app_request_fast_refresh();
}

static void keyboard_settings_auto_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);

    ui_extension_keyboard_set_auto_detect_enabled(
        sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
    keyboard_settings_update_ui();
    app_request_fast_refresh();
}

static void keyboard_settings_interval_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(index >= 0 && index < (int)(sizeof(keyboard_settings_interval_options) /
                                  sizeof(keyboard_settings_interval_options[0]))) {
        ui_extension_keyboard_set_auto_detect_interval_s(
            keyboard_settings_interval_options[index]);
        keyboard_settings_update_ui();
        app_request_fast_refresh();
    }
}

static void keyboard_settings_probe_event_cb(lv_event_t *event)
{
    (void)event;
    ui_extension_keyboard_probe_now();
    keyboard_settings_update_ui();
    app_request_fast_refresh();
}

static void keyboard_settings_hotkey_event_cb(lv_event_t *event)
{
    int f_index = (int)(intptr_t)lv_event_get_user_data(event);
    keyboard_hotkey_action_t action;

    if(f_index < 0 || f_index >= KEYBOARD_HOTKEY_FKEY_COUNT) {
        return;
    }

    action = keyboard_hotkey_next_action(keyboard_hotkey_get_action(f_index));
    keyboard_hotkey_set_action(f_index, action);
    keyboard_settings_update_hotkey_button(f_index);
    app_request_fast_refresh();
}

void ui_keyboard_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *probe;
    lv_obj_t *sw;
    int panel_x = ui_page_panel_x();
    int panel_w = ui_page_panel_width();
    int inner_w = panel_w - 32;
    int button_gap = 12;
    int button_w = (inner_w - button_gap * 2) / 3;
    int hotkey_cols = ui_is_landscape() ? 2 : 1;
    int hotkey_gap = 12;
    int hotkey_cell_w;
    int hotkey_rows;
    int hotkey_panel_h;

    if(button_w < 130) {
        button_w = 130;
    }
    hotkey_cell_w = (inner_w - hotkey_gap * (hotkey_cols - 1)) /
                    hotkey_cols;
    if(hotkey_cell_w < 180) {
        hotkey_cell_w = 180;
    }
    hotkey_rows = (KEYBOARD_HOTKEY_FKEY_COUNT + hotkey_cols - 1) /
                  hotkey_cols;
    hotkey_panel_h = 108 + hotkey_rows * 72;

    ui_create_header(scr, "Keyboard settings");
    body = ui_page_body(scr, 154);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    panel = ui_panel(body, panel_x, 0, panel_w, 220);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x151B22), 0);

    label = ui_label(panel, "Extension keyboard", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    keyboard_settings_status_label =
        ui_label(panel, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(keyboard_settings_status_label, inner_w);
    lv_label_set_long_mode(keyboard_settings_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(keyboard_settings_status_label, LV_ALIGN_TOP_LEFT, 0, 46);

    label = ui_label(panel, "Auto detect keyboard", &lv_font_montserrat_20,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 132);

    label = ui_label(panel, "Probe TCA8418 every interval",
                     &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 166);

    sw = lv_switch_create(panel);
    keyboard_settings_auto_switch = sw;
    lv_obj_set_size(sw, 72, 38);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, 138);
    lv_obj_add_event_cb(sw, keyboard_settings_auto_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    panel = ui_panel(body, panel_x, 244, panel_w, 198);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x151B22), 0);

    label = ui_label(panel, "Detection interval", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    for(size_t i = 0; i < sizeof(keyboard_settings_interval_options) /
           sizeof(keyboard_settings_interval_options[0]); i++) {
        char title[16];

        snprintf(title, sizeof(title), "%d sec",
                 keyboard_settings_interval_options[i]);
        keyboard_settings_interval_btn[i] =
            ui_command_button(panel,
                              (int)i * (button_w + button_gap),
                              54, button_w, title, 0xF97316);
        lv_obj_add_event_cb(keyboard_settings_interval_btn[i],
                            keyboard_settings_interval_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    probe = ui_command_button(panel, 0, 122, inner_w, "Probe now",
                              0x25C281);
    lv_obj_add_event_cb(probe, keyboard_settings_probe_event_cb,
                        LV_EVENT_CLICKED, NULL);

    panel = ui_panel(body, panel_x, 466, panel_w, 130);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x151B22), 0);

    label = ui_label(panel, "Esc key back", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = ui_label(panel, "Use Esc to return to previous page",
                     &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(label, inner_w > 128 ? inner_w - 128 : inner_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 46);

    sw = lv_switch_create(panel);
    keyboard_settings_esc_back_switch = sw;
    lv_obj_set_size(sw, 72, 38);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, 34);
    lv_obj_add_event_cb(sw, keyboard_settings_esc_back_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    ui_settings_nav_row(body, 620, "KEY", "Keyboard test",
                        "Backlight and key test", 0xF97316,
                        PAGE_KEYBOARD_TEST);

    panel = ui_panel(body, panel_x, 738, panel_w, hotkey_panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x151B22), 0);

    label = ui_label(panel, "F-key hotkeys", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = ui_label(panel,
                     "Tap each function key to choose its shortcut action",
                     &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(label, inner_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 38);

    for(int i = 0; i < KEYBOARD_HOTKEY_FKEY_COUNT; i++) {
        int row = i / hotkey_cols;
        int col = i % hotkey_cols;
        int x = col * (hotkey_cell_w + hotkey_gap);
        int y = 86 + row * 72;

        keyboard_settings_hotkey_btn[i] =
            ui_command_button(panel, x, y, hotkey_cell_w,
                              keyboard_hotkey_fkeys[i].name, 0x3DA5FF);
        lv_obj_add_event_cb(keyboard_settings_hotkey_btn[i],
                            keyboard_settings_hotkey_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    keyboard_settings_update_ui();
}

static void audio_update_page(void)
{
    char output[24];
    char text[160];
    int external;
    int volume;
    int max_volume;
    int percent;

    read_pref_text(PREF_AUDIO_OUTPUT, output, sizeof(output),
                   audio_default_output());
    external = strcmp(output, AUDIO_OUTPUT_EXTERNAL) == 0;
    volume = ui_audio_get_volume_value();
    max_volume = ui_audio_get_volume_max();
    percent = max_volume > 0 ? (volume * 100 + max_volume / 2) / max_volume : 0;

    style_choice_button(audio_headphones_btn, !external, 0x3DA5FF);
    style_choice_button(audio_external_btn, external, 0x25C281);
    if(audio_settings_volume_slider &&
       lv_slider_get_value(audio_settings_volume_slider) != volume) {
        lv_slider_set_value(audio_settings_volume_slider, volume, LV_ANIM_OFF);
    }
    if(audio_settings_volume_label) {
        snprintf(text, sizeof(text), "%d%%", percent);
        lv_label_set_text(audio_settings_volume_label, text);
    }
    if(audio_status_label) {
        snprintf(text, sizeof(text), "%s  GPIO34=%s  route=%s",
                 external ? ui_tr("External speaker") : ui_tr("Headphones"),
                 external ? "high" : "low",
                 audio_external_route_supported == 0 ? "missing" :
                 (external ? "external" : "codec"));
        lv_label_set_text(audio_status_label, text);
    }
}

static void audio_output_event_cb(lv_event_t *event)
{
    const char *output = (const char *)lv_event_get_user_data(event);

    if(!output) {
        return;
    }
    ui_prefs_set(PREF_AUDIO_OUTPUT, output);
    audio_apply_output(output);
    audio_update_page();
    app_request_fast_refresh();
}

static void audio_settings_volume_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *slider = lv_event_get_target(event);

    if(!slider) {
        return;
    }
    if(code == LV_EVENT_VALUE_CHANGED) {
        ui_audio_set_volume_value((int)lv_slider_get_value(slider), 0);
        audio_update_page();
    } else if(code == LV_EVENT_RELEASED) {
        ui_audio_set_volume_value((int)lv_slider_get_value(slider), 1);
        audio_update_page();
    }
}

static int hardware_content_width(lv_obj_t *parent, int inset)
{
    int w = ui_safe_content_width(parent, 488);

    w -= inset * 2;
    return w > 240 ? w : 240;
}

static void audio_settings_add_volume(lv_obj_t *body, int y, int x, int w)
{
    lv_obj_t *slider;
    lv_obj_t *title;
    int group_w = w * 85 / 100;
    int group_x;
    int slider_x;
    int slider_w;
    int value_w;

    if(group_w > w) {
        group_w = w;
    }
    if(group_w < 260) {
        group_w = w > 260 ? 260 : w;
    }
    group_x = x;
    slider_x = group_x;
    slider_w = group_w;
    value_w = ui_is_landscape() ? 180 : 144;
    if(value_w > group_w / 2) {
        value_w = group_w / 2;
    }

    title = ui_label(body, "Volume", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, group_x, y);
    lv_obj_set_width(title, group_w - value_w - 12);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    audio_settings_volume_label = ui_label(body, "--", &lv_font_montserrat_18,
                                           0x3DA5FF);
    lv_obj_set_pos(audio_settings_volume_label,
                   group_x + group_w - value_w, y + 2);
    lv_obj_set_width(audio_settings_volume_label, value_w);
    lv_obj_set_style_text_align(audio_settings_volume_label,
                                LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(audio_settings_volume_label, LV_LABEL_LONG_DOT);

    slider = lv_slider_create(body);
    audio_settings_volume_slider = slider;
    lv_obj_set_pos(slider, slider_x, y + 54);
    lv_obj_set_size(slider, slider_w, 24);
    lv_slider_set_range(slider, 0, ui_audio_get_volume_max());
    lv_slider_set_value(slider, ui_audio_get_volume_value(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A3037), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3DA5FF),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F5F8), LV_PART_KNOB);
    lv_obj_set_style_height(slider, 24, LV_PART_MAIN);
    lv_obj_set_style_width(slider, 30, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 30, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, audio_settings_volume_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, audio_settings_volume_event_cb,
                        LV_EVENT_RELEASED, NULL);
}

void ui_audio_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *title;
    int inset = 20;
    int content_w;
    int button_gap = 16;
    int button_w;

    ui_create_header(scr, "Audio");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    content_w = hardware_content_width(body, inset);
    button_w = (content_w - button_gap) / 2;
    if(button_w > 280) {
        button_w = 280;
    }

    title = ui_label(body, "Audio settings", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, inset, 0);

    audio_settings_add_volume(body, 70, inset, content_w);

    audio_status_label = ui_label(body, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(audio_status_label, content_w);
    lv_label_set_long_mode(audio_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(audio_status_label, LV_ALIGN_TOP_LEFT, inset, 188);

    audio_headphones_btn = ui_command_button(body, inset, 256, button_w,
                                             "Headphones", 0x3DA5FF);
    lv_obj_add_event_cb(audio_headphones_btn, audio_output_event_cb,
                        LV_EVENT_CLICKED, (void *)AUDIO_OUTPUT_HEADPHONES);
    audio_external_btn = ui_command_button(body, inset + button_w + button_gap,
                                           256, button_w,
                                           "External speaker", 0x25C281);
    lv_obj_add_event_cb(audio_external_btn, audio_output_event_cb,
                        LV_EVENT_CLICKED, (void *)AUDIO_OUTPUT_EXTERNAL);

    ui_info_row_inset(body, 366, "I2S pins", "BCLK32 LRCK33 DATA35",
                      0xF2F5F8, inset);
    ui_info_row_inset(body, 420, "Amp shutdown", "GPIO34 high enable",
                      0xF2F5F8, inset);
    ui_info_row_inset(body, 474, "ALSA card", "K230_I2S_INNO", 0x25C281,
                      inset);
    ui_info_row_inset(body, 528, "Mixer", AUDIO_EXTERNAL_I2S_CONTROL,
                      0x9AA4AF, inset);

    audio_update_page();
}

void ui_audio_output_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *title;

    ui_create_header(scr, "Audio output");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    title = ui_label(body, "Route", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    audio_status_label = ui_label(body, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(audio_status_label, ui_inner_width());
    lv_label_set_long_mode(audio_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(audio_status_label, LV_ALIGN_TOP_LEFT, 0, 48);

    audio_headphones_btn = ui_command_button(body, 0, 118, 236, "Headphones",
                                             0x3DA5FF);
    lv_obj_add_event_cb(audio_headphones_btn, audio_output_event_cb,
                        LV_EVENT_CLICKED, (void *)AUDIO_OUTPUT_HEADPHONES);

    audio_external_btn = ui_command_button(body, 252, 118, 236,
                                           "External speaker", 0x25C281);
    lv_obj_add_event_cb(audio_external_btn, audio_output_event_cb,
                        LV_EVENT_CLICKED, (void *)AUDIO_OUTPUT_EXTERNAL);

    ui_info_row(body, 226, "I2S pins", "BCLK32 LRCK33 DATA35", 0xF2F5F8);
    ui_info_row(body, 280, "Amp shutdown", "GPIO34 high enable", 0xF2F5F8);
    ui_info_row(body, 334, "ALSA card", "K230_I2S_INNO", 0x25C281);
    ui_info_row(body, 388, "Mixer", AUDIO_EXTERNAL_I2S_CONTROL, 0x9AA4AF);

    audio_update_page();
}

static void fan_update_button_state(const char *mode)
{
    style_choice_button(fan_off_btn, strcmp(mode, FAN_MODE_OFF) == 0, 0x9AA4AF);
    style_choice_button(fan_on_btn, strcmp(mode, FAN_MODE_ON) == 0, 0x25C281);
    style_choice_button(fan_auto_btn, strcmp(mode, FAN_MODE_AUTO) == 0, 0xF5A524);
}

static void fan_update_page(void)
{
    char mode[16];
    char text[128];
    char source[64];
    double temp = 0.0;
    int duty = read_pref_int(PREF_FAN_DUTY, 60, 0, 100);
    int on_temp = read_pref_int(PREF_FAN_ON_TEMP, 55, 20, 100);
    int off_temp = read_pref_int(PREF_FAN_OFF_TEMP, 45, 10, 95);
    int last_duty;

    read_pref_text(PREF_FAN_MODE, mode, sizeof(mode), FAN_MODE_OFF);
    fan_update_button_state(mode);

    pthread_mutex_lock(&hardware_lock);
    last_duty = fan_last_duty;
    pthread_mutex_unlock(&hardware_lock);

    if(fan_mode_label) {
        lv_label_set_text(fan_mode_label, ui_tr(mode));
    }
    if(fan_duty_label) {
        snprintf(text, sizeof(text), "%d%%", duty);
        lv_label_set_text(fan_duty_label, text);
    }
    if(fan_on_label) {
        snprintf(text, sizeof(text), "%d C", on_temp);
        lv_label_set_text(fan_on_label, text);
    }
    if(fan_off_label) {
        snprintf(text, sizeof(text), "%d C", off_temp);
        lv_label_set_text(fan_off_label, text);
    }
    if(fan_duty_slider && lv_slider_get_value(fan_duty_slider) != duty) {
        lv_slider_set_value(fan_duty_slider, duty, LV_ANIM_OFF);
    }
    if(fan_on_slider && lv_slider_get_value(fan_on_slider) != on_temp) {
        lv_slider_set_value(fan_on_slider, on_temp, LV_ANIM_OFF);
    }
    if(fan_off_slider && lv_slider_get_value(fan_off_slider) != off_temp) {
        lv_slider_set_value(fan_off_slider, off_temp, LV_ANIM_OFF);
    }

    if(sensor_read_control_temp(&temp, source, sizeof(source)) == 0) {
        snprintf(text, sizeof(text), "%.1f C  %s", temp, source);
    } else {
        snprintf(text, sizeof(text), "No temperature sensor");
    }
    if(fan_temp_label) {
        lv_label_set_text(fan_temp_label, text);
    }
    if(fan_status_label) {
        snprintf(text, sizeof(text), "Output %d%%  PWM0/GPIO42", last_duty < 0 ? 0 :
                 last_duty);
        lv_label_set_text(fan_status_label, text);
    }
}

static void fan_mode_event_cb(lv_event_t *event)
{
    const char *mode = (const char *)lv_event_get_user_data(event);

    if(!mode) {
        return;
    }
    ui_prefs_set(PREF_FAN_MODE, mode);
    fan_apply_policy_once();
    fan_update_page();
}

static void fan_slider_event_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    const char *key = (const char *)lv_event_get_user_data(event);

    if(!slider || !key) {
        return;
    }
    write_pref_int(key, (int)lv_slider_get_value(slider));
    fan_apply_policy_once();
    fan_update_page();
}

static lv_obj_t *create_slider(lv_obj_t *parent, int y, int min_value,
                               int max_value, int value, const char *key)
{
    lv_obj_t *slider = lv_slider_create(parent);

    lv_obj_set_pos(slider, 0, y);
    lv_obj_set_size(slider, ui_fit_width(lv_obj_get_parent(slider), 0, 488), 22);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A3037), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3DA5FF),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F5F8), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, fan_slider_event_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)key);
    return slider;
}

static void hardware_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    fan_update_page();
}

void ui_fan_create(lv_obj_t *scr)
{
    lv_obj_t *body;

    ui_create_header(scr, "Fan");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    if(!K230_FAN_ENABLED) {
        lv_obj_t *title = ui_label(body, "Fan disabled", &lv_font_montserrat_24,
                                   0xF2F5F8);
        lv_obj_t *detail = ui_label(body,
                                    "GPIO42 is reserved for TCA8418 keyboard IRQ.",
                                    &lv_font_montserrat_18, 0xF5A524);
        lv_obj_t *note = ui_label(body,
                                  "The fan control code is kept for a future GPIO/PWM remap.",
                                  &lv_font_montserrat_16, 0x9AA4AF);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_width(detail, ui_inner_width());
        lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
        lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 0, 54);
        lv_obj_set_width(note, ui_inner_width());
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 126);
        return;
    }

    ui_label(body, "Fan control", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    fan_temp_label = ui_label(body, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(fan_temp_label, ui_inner_width());
    lv_label_set_long_mode(fan_temp_label, LV_LABEL_LONG_DOT);
    lv_obj_align(fan_temp_label, LV_ALIGN_TOP_LEFT, 0, 48);

    fan_status_label = ui_label(body, "--", &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_width(fan_status_label, ui_inner_width());
    lv_label_set_long_mode(fan_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(fan_status_label, LV_ALIGN_TOP_LEFT, 0, 86);

    fan_off_btn = ui_command_button(body, 0, 140, 150, "Off", 0x9AA4AF);
    fan_on_btn = ui_command_button(body, 169, 140, 150, "On", 0x25C281);
    fan_auto_btn = ui_command_button(body, 338, 140, 150, "Auto", 0xF5A524);
    lv_obj_add_event_cb(fan_off_btn, fan_mode_event_cb, LV_EVENT_CLICKED,
                        (void *)FAN_MODE_OFF);
    lv_obj_add_event_cb(fan_on_btn, fan_mode_event_cb, LV_EVENT_CLICKED,
                        (void *)FAN_MODE_ON);
    lv_obj_add_event_cb(fan_auto_btn, fan_mode_event_cb, LV_EVENT_CLICKED,
                        (void *)FAN_MODE_AUTO);

    ui_info_row(body, 240, "Mode", "--", 0xF5A524);
    fan_mode_label = lv_obj_get_child(body, lv_obj_get_child_count(body) - 1);

    ui_label(body, "Duty", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 320);
    fan_duty_label = ui_label(body, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(fan_duty_label, LV_ALIGN_TOP_RIGHT, 0, 322);
    fan_duty_slider = create_slider(body, 372, 0, 100,
                                    read_pref_int(PREF_FAN_DUTY, 60, 0, 100),
                                    PREF_FAN_DUTY);

    ui_label(body, "Auto on above", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 450);
    fan_on_label = ui_label(body, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(fan_on_label, LV_ALIGN_TOP_RIGHT, 0, 452);
    fan_on_slider = create_slider(body, 502, 20, 100,
                                  read_pref_int(PREF_FAN_ON_TEMP, 55, 20, 100),
                                  PREF_FAN_ON_TEMP);

    ui_label(body, "Auto off below", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 580);
    fan_off_label = ui_label(body, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(fan_off_label, LV_ALIGN_TOP_RIGHT, 0, 582);
    fan_off_slider = create_slider(body, 632, 10, 95,
                                   read_pref_int(PREF_FAN_OFF_TEMP, 45, 10, 95),
                                   PREF_FAN_OFF_TEMP);

    ui_info_row(body, 724, "Pin", "GPIO42 / PWM0", 0xF2F5F8);

    hardware_page_timer = lv_timer_create(hardware_timer_cb, 1000, NULL);
    fan_update_page();
}

static void sensors_update_page(void)
{
    sensor_reading_t reading;
    double cpu_temp = 0.0;
    char text[128];

    if(sensor_status_label && sensor_read_aht20(&reading) == 0) {
        lv_label_set_text(sensor_status_label, ui_tr("Ready"));
        lv_obj_set_style_text_color(sensor_status_label, lv_color_hex(0x25C281),
                                    0);

        snprintf(text, sizeof(text), "%.1f C", reading.temp_c);
        lv_label_set_text(sensor_temp_label, text);
        snprintf(text, sizeof(text), "%.1f%%", reading.humidity_pct);
        lv_label_set_text(sensor_humidity_label, text);
        lv_label_set_text(sensor_bus_label, reading.source);
    } else if(sensor_status_label) {
        lv_label_set_text(sensor_status_label, ui_tr("AHT20 not detected"));
        lv_obj_set_style_text_color(sensor_status_label, lv_color_hex(0xF5A524),
                                    0);
        lv_label_set_text(sensor_temp_label, "--");
        lv_label_set_text(sensor_humidity_label, "--");
        lv_label_set_text(sensor_bus_label, "/dev/i2c-0 0x38");
    }

    if(sensor_cpu_label) {
        if(sensor_read_cpu_temp(&cpu_temp) == 0) {
            snprintf(text, sizeof(text), "%.1f C", cpu_temp);
        } else {
            snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(sensor_cpu_label, text);
    }
}

static void sensors_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    sensors_update_page();
}

static void bq25896_update_page(void)
{
    keyboard_base_state_t base;
    bq25896_reading_t reading;
    char text[160];

    keyboard_base_get_state(&base);

    if(bq25896_base_label) {
        lv_label_set_text(bq25896_base_label, base.scanned ?
                          base.status : "Keyboard base not scanned");
    }

    if(bq25896_read(&reading) == 0) {
        if(bq25896_status_label) {
            snprintf(text, sizeof(text), "%s  PN=%d REV=%d",
                     bq25896_bus_state_name(reading.status_reg),
                     reading.part_number, reading.revision);
            lv_label_set_text(bq25896_status_label, text);
            lv_obj_set_style_text_color(bq25896_status_label,
                                        lv_color_hex(0x25C281), 0);
        }
        if(bq25896_charge_label) {
            snprintf(text, sizeof(text), "%s  %s",
                     bq25896_charge_state_name(reading.status_reg),
                     reading.charge_enabled ? "enabled" : "disabled");
            lv_label_set_text(bq25896_charge_label, text);
        }
        if(bq25896_slider_label) {
            snprintf(text, sizeof(text), "%d mA", reading.fast_charge_ma);
            lv_label_set_text(bq25896_slider_label, text);
        }
        if(bq25896_current_slider &&
           lv_slider_get_value(bq25896_current_slider) != reading.fast_charge_ma) {
            lv_slider_set_value(bq25896_current_slider,
                                reading.fast_charge_ma, LV_ANIM_OFF);
        }
        if(bq25896_input_label) {
            snprintf(text, sizeof(text), "%d mA", reading.input_limit_ma);
            lv_label_set_text(bq25896_input_label, text);
        }
        if(bq25896_voltage_label) {
            snprintf(text, sizeof(text), "%d mV", reading.charge_voltage_mv);
            lv_label_set_text(bq25896_voltage_label, text);
        }
        if(bq25896_vbat_label) {
            snprintf(text, sizeof(text), "%d mV", reading.vbat_mv);
            lv_label_set_text(bq25896_vbat_label, text);
        }
        if(bq25896_vsys_label) {
            snprintf(text, sizeof(text), "%d mV", reading.vsys_mv);
            lv_label_set_text(bq25896_vsys_label, text);
        }
        if(bq25896_vbus_label) {
            snprintf(text, sizeof(text), "%d mV", reading.vbus_mv);
            lv_label_set_text(bq25896_vbus_label, text);
        }
        if(bq25896_ichg_label) {
            snprintf(text, sizeof(text), "%d mA", reading.charge_current_ma);
            lv_label_set_text(bq25896_ichg_label, text);
        }
        if(bq25896_ntc_label) {
            snprintf(text, sizeof(text), "%.1f%%", reading.ntc_pct);
            lv_label_set_text(bq25896_ntc_label, text);
        }
        if(bq25896_fault_label) {
            snprintf(text, sizeof(text), "0x%02X  IDPM 0x%02X",
                     reading.fault_reg, reading.idpm_reg);
            lv_label_set_text(bq25896_fault_label, text);
        }
        style_choice_button(bq25896_charge_on_btn, reading.charge_enabled,
                            0x25C281);
        style_choice_button(bq25896_charge_off_btn, !reading.charge_enabled,
                            0xEF4D5A);
    } else {
        if(bq25896_status_label) {
            lv_label_set_text(bq25896_status_label, ui_tr("BQ25896 not detected"));
            lv_obj_set_style_text_color(bq25896_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
        if(bq25896_charge_label) {
            lv_label_set_text(bq25896_charge_label, "--");
        }
        if(bq25896_slider_label) {
            lv_label_set_text(bq25896_slider_label, "--");
        }
        if(bq25896_input_label) {
            lv_label_set_text(bq25896_input_label, "--");
        }
        if(bq25896_voltage_label) {
            lv_label_set_text(bq25896_voltage_label, "--");
        }
        if(bq25896_vbat_label) {
            lv_label_set_text(bq25896_vbat_label, "--");
        }
        if(bq25896_vsys_label) {
            lv_label_set_text(bq25896_vsys_label, "--");
        }
        if(bq25896_vbus_label) {
            lv_label_set_text(bq25896_vbus_label, "--");
        }
        if(bq25896_ichg_label) {
            lv_label_set_text(bq25896_ichg_label, "--");
        }
        if(bq25896_ntc_label) {
            lv_label_set_text(bq25896_ntc_label, "--");
        }
        if(bq25896_fault_label) {
            lv_label_set_text(bq25896_fault_label, "--");
        }
        style_choice_button(bq25896_charge_on_btn, 0, 0x25C281);
        style_choice_button(bq25896_charge_off_btn, 0, 0xEF4D5A);
    }
}

static void bq25896_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bq25896_update_page();
}

static void bq25896_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    keyboard_base_probe();
    bq25896_update_page();
}

static void bq25896_charge_enable_event_cb(lv_event_t *event)
{
    int enable = (int)(intptr_t)lv_event_get_user_data(event);

    bq25896_set_charge_enabled(enable);
    bq25896_update_page();
    app_request_fast_refresh();
}

static void bq25896_current_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *slider = lv_event_get_target(event);
    int ma;

    if(!slider) {
        return;
    }
    ma = (int)lv_slider_get_value(slider);
    if(bq25896_slider_label) {
        char text[32];
        snprintf(text, sizeof(text), "%d mA", ma);
        lv_label_set_text(bq25896_slider_label, text);
    }
    if(code == LV_EVENT_RELEASED) {
        bq25896_set_fast_charge_current(ma);
        bq25896_update_page();
        app_request_fast_refresh();
    }
}

static void battery_format_minutes(char *buf, size_t len, int minutes)
{
    if(!buf || len == 0) {
        return;
    }
    if(minutes <= 0 || minutes >= 65535) {
        snprintf(buf, len, "--");
        return;
    }
    snprintf(buf, len, "%dh %02dm", minutes / 60, minutes % 60);
}

static void battery_update_page(void)
{
    keyboard_base_state_t base;
    bq27220_reading_t reading;
    char text[160];
    char time_text[32];
    int soc;

    keyboard_base_get_state(&base);
    if(battery_base_label) {
        lv_label_set_text(battery_base_label, base.scanned ?
                          base.status : "Keyboard base not scanned");
    }

    if(bq27220_read(&reading) == 0) {
        soc = clamp_int(reading.soc_pct, 0, 100);
        if(battery_status_label) {
            snprintf(text, sizeof(text), "%s  status=0x%04X",
                     bq27220_flow_state(&reading), reading.battery_status);
            lv_label_set_text(battery_status_label, text);
            lv_obj_set_style_text_color(battery_status_label,
                                        lv_color_hex(0x25C281), 0);
        }
        if(battery_soc_label) {
            snprintf(text, sizeof(text), "%d%%", soc);
            lv_label_set_text(battery_soc_label, text);
        }
        if(battery_soc_bar) {
            lv_bar_set_value(battery_soc_bar, soc, LV_ANIM_ON);
        }
        if(battery_voltage_label) {
            snprintf(text, sizeof(text), "%d mV", reading.voltage_mv);
            lv_label_set_text(battery_voltage_label, text);
        }
        if(battery_current_label) {
            snprintf(text, sizeof(text), "%d mA", reading.current_ma);
            lv_label_set_text(battery_current_label, text);
        }
        if(battery_remaining_label) {
            snprintf(text, sizeof(text), "%d mAh", reading.remaining_mah);
            lv_label_set_text(battery_remaining_label, text);
        }
        if(battery_full_label) {
            snprintf(text, sizeof(text), "%d mAh", reading.full_charge_mah);
            lv_label_set_text(battery_full_label, text);
        }
        if(battery_design_label) {
            snprintf(text, sizeof(text), "%d mAh", reading.design_mah);
            lv_label_set_text(battery_design_label, text);
        }
        if(battery_temp_label) {
            snprintf(text, sizeof(text), "%.1f C", reading.temp_c);
            lv_label_set_text(battery_temp_label, text);
        }
        if(battery_internal_temp_label) {
            snprintf(text, sizeof(text), "%.1f C", reading.internal_temp_c);
            lv_label_set_text(battery_internal_temp_label, text);
        }
        if(battery_time_empty_label) {
            battery_format_minutes(time_text, sizeof(time_text),
                                   reading.time_to_empty_min);
            lv_label_set_text(battery_time_empty_label, time_text);
        }
        if(battery_time_full_label) {
            battery_format_minutes(time_text, sizeof(time_text),
                                   reading.time_to_full_min);
            lv_label_set_text(battery_time_full_label, time_text);
        }
        if(battery_health_label) {
            snprintf(text, sizeof(text), "%d%%", reading.soh_pct);
            lv_label_set_text(battery_health_label, text);
        }
        if(battery_cycle_label) {
            snprintf(text, sizeof(text), "%d", reading.cycle_count);
            lv_label_set_text(battery_cycle_label, text);
        }
        if(battery_power_label) {
            snprintf(text, sizeof(text), "%d mW", reading.average_power_mw);
            lv_label_set_text(battery_power_label, text);
        }
        if(battery_status_reg_label) {
            snprintf(text, sizeof(text), "op=0x%04X req=%dmV/%dmA",
                     reading.operation_status, reading.charging_voltage_mv,
                     reading.charging_current_ma);
            lv_label_set_text(battery_status_reg_label, text);
        }
    } else {
        if(battery_status_label) {
            lv_label_set_text(battery_status_label, ui_tr("BQ27220 not detected"));
            lv_obj_set_style_text_color(battery_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
        if(battery_soc_label) {
            lv_label_set_text(battery_soc_label, "--");
        }
        if(battery_soc_bar) {
            lv_bar_set_value(battery_soc_bar, 0, LV_ANIM_OFF);
        }
        if(battery_voltage_label) {
            lv_label_set_text(battery_voltage_label, "--");
        }
        if(battery_current_label) {
            lv_label_set_text(battery_current_label, "--");
        }
        if(battery_remaining_label) {
            lv_label_set_text(battery_remaining_label, "--");
        }
        if(battery_full_label) {
            lv_label_set_text(battery_full_label, "--");
        }
        if(battery_design_label) {
            lv_label_set_text(battery_design_label, "--");
        }
        if(battery_temp_label) {
            lv_label_set_text(battery_temp_label, "--");
        }
        if(battery_internal_temp_label) {
            lv_label_set_text(battery_internal_temp_label, "--");
        }
        if(battery_time_empty_label) {
            lv_label_set_text(battery_time_empty_label, "--");
        }
        if(battery_time_full_label) {
            lv_label_set_text(battery_time_full_label, "--");
        }
        if(battery_health_label) {
            lv_label_set_text(battery_health_label, "--");
        }
        if(battery_cycle_label) {
            lv_label_set_text(battery_cycle_label, "--");
        }
        if(battery_power_label) {
            lv_label_set_text(battery_power_label, "--");
        }
        if(battery_status_reg_label) {
            lv_label_set_text(battery_status_reg_label, "--");
        }
    }
}

static void battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    battery_update_page();
}

static void battery_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    keyboard_base_probe();
    battery_update_page();
}

static void battery_info_row(lv_obj_t *parent, int y, const char *name,
                             const char *value, uint32_t value_color)
{
    int row_w = parent ? lv_obj_get_content_width(parent) : 0;
    int side_pad = ui_is_landscape() ? 34 : 24;
    int value_w;
    lv_obj_t *left;
    lv_obj_t *right;

    if(row_w <= 0) {
        row_w = ui_fit_width(parent, 0, 488);
    }
    value_w = row_w - 196 - side_pad * 2;
    if(value_w < 160) {
        value_w = 160;
    }
    if(value_w > 280) {
        value_w = 280;
    }

    left = ui_label(parent, name, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(left, row_w - value_w - side_pad * 2 - 24);
    lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, side_pad, y);

    right = ui_label(parent, value, &lv_font_montserrat_20, value_color);
    lv_obj_set_width(right, value_w);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -side_pad, y - 2);
}

static void keyboard_update_grid(void)
{
    for(int row = 0; row < KEYBOARD_LAYOUT_ROWS; row++) {
        for(int col = 0; col < KEYBOARD_LAYOUT_COLS; col++) {
            lv_obj_t *box = keyboard_key_box[row][col];
            lv_obj_t *label = keyboard_key_label[row][col];
            int code = keyboard_display_layout[row][col];
            int pressed = keyboard_code_is_pressed(code);

            if(!box) {
                continue;
            }
            lv_obj_set_style_bg_color(box,
                                      lv_color_hex(pressed ? 0x25C281 :
                                                   0x202832), 0);
            lv_obj_set_style_border_color(box,
                                          lv_color_hex(pressed ? 0xA3E635 :
                                                       0x2A3037), 0);
            if(label) {
                lv_obj_set_style_text_color(label,
                                            lv_color_hex(pressed ? 0xFFFFFF :
                                                         0x9AA4AF), 0);
            }
        }
    }
}

static void keyboard_update_page(void)
{
    keyboard_base_state_t base;
    int fifo_count;
    unsigned int irq_count;
    unsigned int irq_errors;
    unsigned int safety_count;
    unsigned int overflow_count;
    unsigned int total_events;
    uint64_t irq_last_us;
    char last_event[sizeof(keyboard_last_event_text)];
    char text[160];

    keyboard_base_get_state(&base);
    if(keyboard_base_label) {
        int base_ready = base.bq25896 || base.bq27220 ||
                         base.tca8418 || base.xl9555;

        lv_label_set_text(keyboard_base_label,
                          base.scanned && base_ready ? "Ready" : "Not ready");
        lv_obj_set_style_text_color(keyboard_base_label,
                                    lv_color_hex(base.scanned && base_ready ?
                                                 0x25C281 : 0xF5A524), 0);
    }

    pthread_mutex_lock(&tca8418_event_lock);
    fifo_count = tca8418_last_fifo_count;
    irq_count = tca8418_irq_count;
    irq_errors = tca8418_irq_error_count;
    safety_count = tca8418_irq_safety_count;
    overflow_count = tca8418_overflow_count;
    irq_last_us = tca8418_irq_last_us;
    total_events = keyboard_event_total;
    snprintf(last_event, sizeof(last_event), "%s", keyboard_last_event_text);
    pthread_mutex_unlock(&tca8418_event_lock);

    if(keyboard_status_label) {
        if(keyboard_tca8418_ready) {
            lv_label_set_text(keyboard_status_label, "Ready");
            lv_obj_set_style_text_color(keyboard_status_label,
                                        lv_color_hex(0x25C281), 0);
        } else {
            lv_label_set_text(keyboard_status_label,
                              ui_tr("TCA8418 not detected"));
            lv_obj_set_style_text_color(keyboard_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
    }
    if(keyboard_last_label) {
        lv_label_set_text(keyboard_last_label, ui_tr(last_event));
    }
    if(keyboard_count_label) {
        snprintf(text, sizeof(text),
                 "fifo=%d total=%u irq=%u safe=%u ovr=%u err=%u last=%llums",
                 fifo_count, total_events, irq_count, safety_count,
                 overflow_count, irq_errors,
                 irq_last_us ? (unsigned long long)(ui_monotonic_us() -
                               irq_last_us) / 1000ULL : 0ULL);
        lv_label_set_text(keyboard_count_label, text);
    }
    keyboard_update_grid();
}

static void keyboard_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    keyboard_update_page();
}

static void keyboard_clear_event_cb(lv_event_t *event)
{
    (void)event;
    pthread_mutex_lock(&tca8418_event_lock);
    memset(keyboard_key_pressed, 0, sizeof(keyboard_key_pressed));
    keyboard_event_total = 0;
    tca8418_last_fifo_count = 0;
    tca8418_irq_safety_count = 0;
    tca8418_overflow_count = 0;
    tca8418_reset_dispatch_requested = 1;
    snprintf(keyboard_last_event_text, sizeof(keyboard_last_event_text),
             "No event");
    pthread_mutex_unlock(&tca8418_event_lock);
    tca8418_raw_queue_clear();
    extension_keyboard_queue_clear();
    if(keyboard_tca8418_ready) {
        tca8418_flush_events();
    }
    keyboard_update_page();
}

static void keyboard_backlight_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);

    keyboard_backlight_apply((int)lv_slider_get_value(slider), 0, 1);
}

static void keyboard_backlight_slider_key_cb(lv_event_t *event)
{
    uint32_t key = lv_event_get_key(event);

    if(key == LV_KEY_UP || key == LV_KEY_DOWN ||
       key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        lv_event_stop_processing(event);
    }
}

static void xl9555_update_page(void)
{
    keyboard_base_state_t base;
    uint8_t input0 = 0;
    uint8_t output0 = 0;
    uint8_t config0 = 0;
    char text[160];

    keyboard_base_get_state(&base);
    if(xl9555_base_label) {
        lv_label_set_text(xl9555_base_label, base.scanned ?
                          base.status : ui_tr("Keyboard base not scanned"));
    }

    if(!xl9555_ready && xl9555_init_device() != 0) {
        if(xl9555_status_label) {
            lv_label_set_text(xl9555_status_label,
                              ui_tr("XL9555 not detected"));
            lv_obj_set_style_text_color(xl9555_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
        if(xl9555_output_label) {
            lv_label_set_text(xl9555_output_label, "--");
        }
        if(xl9555_config_label) {
            lv_label_set_text(xl9555_config_label, "--");
        }
        for(size_t i = 0; i < 3; i++) {
            style_choice_button(xl9555_led_btn[i], 0, 0x22D3EE);
        }
        return;
    }

    if(xl9555_read_regs(&input0, &output0, &config0) != 0) {
        xl9555_ready = 0;
        if(xl9555_status_label) {
            lv_label_set_text(xl9555_status_label,
                              ui_tr("XL9555 not detected"));
            lv_obj_set_style_text_color(xl9555_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
        return;
    }

    xl9555_led_state[0] =
        (output0 & (uint8_t)(1U << XL9555_LED_P03_BIT)) ? 0 : 1;
    xl9555_led_state[1] =
        (output0 & (uint8_t)(1U << XL9555_LED_P04_BIT)) ? 0 : 1;
    xl9555_led_state[2] =
        (output0 & (uint8_t)(1U << XL9555_LED_P05_BIT)) ? 0 : 1;

    if(xl9555_status_label) {
        snprintf(text, sizeof(text), "Ready  addr=0x%02X  active-low LEDs",
                 XL9555_ADDR);
        lv_label_set_text(xl9555_status_label, text);
        lv_obj_set_style_text_color(xl9555_status_label,
                                    lv_color_hex(0x25C281), 0);
    }
    if(xl9555_output_label) {
        snprintf(text, sizeof(text), "OUT0=0x%02X  IN0=0x%02X", output0,
                 input0);
        lv_label_set_text(xl9555_output_label, text);
    }
    if(xl9555_config_label) {
        snprintf(text, sizeof(text), "CFG0=0x%02X  LED mask=0x%02X",
                 config0, XL9555_LED_MASK);
        lv_label_set_text(xl9555_config_label, text);
    }
    for(size_t i = 0; i < 3; i++) {
        style_choice_button(xl9555_led_btn[i], xl9555_led_state[i],
                            0x22D3EE);
    }
}

static void xl9555_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    xl9555_update_page();
}

static void xl9555_led_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(index < 0 || index >= 3) {
        return;
    }
    if(xl9555_set_led(index, !xl9555_led_state[index]) != 0) {
        xl9555_ready = 0;
    }
    xl9555_update_page();
    app_request_fast_refresh();
}

static void xl9555_all_event_cb(lv_event_t *event)
{
    int enabled = (int)(intptr_t)lv_event_get_user_data(event);

    if(xl9555_set_all(enabled) != 0) {
        xl9555_ready = 0;
    }
    xl9555_update_page();
    app_request_fast_refresh();
}

void ui_sensors_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    int inset = 20;
    int content_w;

    ui_create_header(scr, "Sensors");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    content_w = hardware_content_width(body, inset);

    ui_label(body, "AHT20", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, inset, 0);

    sensor_status_label = ui_label(body, "--", &lv_font_montserrat_20,
                                   0x9AA4AF);
    lv_obj_set_width(sensor_status_label, content_w);
    lv_label_set_long_mode(sensor_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(sensor_status_label, LV_ALIGN_TOP_LEFT, inset, 52);

    ui_info_row_inset(body, 122, "Temperature", "--", 0x25C281, inset);
    sensor_temp_label = lv_obj_get_child(body, lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 176, "Humidity", "--", 0x3DA5FF, inset);
    sensor_humidity_label = lv_obj_get_child(body,
                                             lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 230, "I2C", "I2C4 SDA47/SCL46", 0xF2F5F8,
                      inset);
    sensor_bus_label = lv_obj_get_child(body, lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 284, "CPU temp", "--", 0xF5A524, inset);
    sensor_cpu_label = lv_obj_get_child(body, lv_obj_get_child_count(body) - 1);

    ui_info_row_inset(body, 360, "Expected addr", "0x38", 0xF2F5F8,
                      inset);
    ui_info_row_inset(body, 414, "Current scan", "fixed /dev/i2c-0 only",
                      0x9AA4AF, inset);

    hardware_page_timer = lv_timer_create(sensors_timer_cb, 1200, NULL);
    sensors_update_page();
}

void ui_bq25896_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *current_title;
    int last_current;
    int inset = 20;
    int content_w;
    int button_gap = 16;
    int button_w;
    int group_x;
    int group_w;
    int slider_x;
    int slider_w;
    int value_w;

    ui_create_header(scr, "Charger");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    content_w = hardware_content_width(body, inset);
    group_w = content_w * 85 / 100;
    if(group_w > content_w) {
        group_w = content_w;
    }
    if(group_w < 260) {
        group_w = content_w > 260 ? 260 : content_w;
    }
    group_x = inset;
    slider_x = group_x;
    slider_w = group_w;
    value_w = ui_is_landscape() ? 220 : 160;
    if(value_w > group_w / 2) {
        value_w = group_w / 2;
    }
    button_w = (content_w - button_gap) / 2;
    if(button_w > 280) {
        button_w = 280;
    }

    ui_label(body, "BQ25896", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, inset, 0);

    bq25896_base_label = ui_label(body, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(bq25896_base_label, content_w);
    lv_label_set_long_mode(bq25896_base_label, LV_LABEL_LONG_DOT);
    lv_obj_align(bq25896_base_label, LV_ALIGN_TOP_LEFT, inset, 48);

    bq25896_status_label = ui_label(body, "--", &lv_font_montserrat_20,
                                    0x9AA4AF);
    lv_obj_set_width(bq25896_status_label, content_w);
    lv_label_set_long_mode(bq25896_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(bq25896_status_label, LV_ALIGN_TOP_LEFT, inset, 86);

    ui_info_row_inset(body, 148, "Charge", "--", 0x25C281, inset);
    bq25896_charge_label = lv_obj_get_child(body,
                                            lv_obj_get_child_count(body) - 1);

    bq25896_charge_on_btn = ui_command_button(body, inset, 208, button_w,
                                              "Enable", 0x25C281);
    bq25896_charge_off_btn = ui_command_button(body,
                                               inset + button_w + button_gap,
                                               208, button_w, "Disable",
                                               0xEF4D5A);
    lv_obj_add_event_cb(bq25896_charge_on_btn, bq25896_charge_enable_event_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_add_event_cb(bq25896_charge_off_btn, bq25896_charge_enable_event_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)0);

    current_title = ui_label(body, "Fast charge current",
                             &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(current_title, group_x, 298);
    lv_obj_set_width(current_title, group_w - value_w - 12);
    lv_label_set_long_mode(current_title, LV_LABEL_LONG_DOT);
    bq25896_slider_label = ui_label(body, "--", &lv_font_montserrat_18,
                                    0xF97316);
    lv_obj_set_width(bq25896_slider_label, value_w);
    lv_obj_set_style_text_align(bq25896_slider_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(bq25896_slider_label,
                   group_x + group_w - value_w, 300);

    last_current = read_pref_int(PREF_BQ25896_ICHG_MA, 512, 0,
                                 BQ25896_FAST_CHG_MAX_MA);
    bq25896_current_slider = lv_slider_create(body);
    lv_obj_set_pos(bq25896_current_slider, slider_x, 354);
    lv_obj_set_size(bq25896_current_slider, slider_w, 24);
    lv_slider_set_range(bq25896_current_slider, 0, BQ25896_FAST_CHG_MAX_MA);
    lv_slider_set_value(bq25896_current_slider, last_current, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bq25896_current_slider, lv_color_hex(0x2A3037),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(bq25896_current_slider, lv_color_hex(0xF97316),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bq25896_current_slider, lv_color_hex(0xF2F5F8),
                              LV_PART_KNOB);
    lv_obj_set_style_height(bq25896_current_slider, 24, LV_PART_MAIN);
    lv_obj_set_style_width(bq25896_current_slider, 30, LV_PART_KNOB);
    lv_obj_set_style_height(bq25896_current_slider, 30, LV_PART_KNOB);
    lv_obj_add_event_cb(bq25896_current_slider, bq25896_current_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bq25896_current_slider, bq25896_current_event_cb,
                        LV_EVENT_RELEASED, NULL);

    ui_info_row_inset(body, 430, "Input limit", "--", 0xF2F5F8, inset);
    bq25896_input_label = lv_obj_get_child(body,
                                           lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 484, "Charge voltage", "--", 0xF2F5F8, inset);
    bq25896_voltage_label = lv_obj_get_child(body,
                                             lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 558, "VBAT", "--", 0x25C281, inset);
    bq25896_vbat_label = lv_obj_get_child(body,
                                          lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 612, "VSYS", "--", 0x3DA5FF, inset);
    bq25896_vsys_label = lv_obj_get_child(body,
                                          lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 666, "VBUS", "--", 0xF5A524, inset);
    bq25896_vbus_label = lv_obj_get_child(body,
                                          lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 720, "Charge ADC", "--", 0xF97316, inset);
    bq25896_ichg_label = lv_obj_get_child(body,
                                          lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 774, "NTC", "--", 0x22D3EE, inset);
    bq25896_ntc_label = lv_obj_get_child(body,
                                         lv_obj_get_child_count(body) - 1);
    ui_info_row_inset(body, 828, "Fault", "--", 0xEF4D5A, inset);
    bq25896_fault_label = lv_obj_get_child(body,
                                           lv_obj_get_child_count(body) - 1);

    hardware_page_timer = lv_timer_create(bq25896_timer_cb, 1500, NULL);
    bq25896_update_page();
}

void ui_battery_monitor_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *summary;
    lv_obj_t *metrics_a;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(154);
    int body_w = ui_screen_width() - 48;
    int left_w = landscape ? 300 : 520;
    int details_x = landscape ? left_w + 24 : 0;
    int details_w = landscape ? body_w - details_x : 520;

    if(details_w < 320) {
        details_w = body_w - details_x;
    }
    if(details_w < 260) {
        details_w = 260;
    }

    ui_create_header(scr, "Battery");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_bottom(body, 96, 0);
    if(landscape) {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    } else {
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
    }

    if(landscape) {
        summary = ui_panel(body, 0, 0, left_w, body_h);
        metrics_a = ui_scroll_panel(body, details_x, 0, details_w, body_h);
    } else {
        summary = body;
        metrics_a = body;
    }

    ui_label(summary, "BQ27220", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(summary, lv_obj_get_child_count(summary) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    battery_base_label = ui_label(summary, "--", &lv_font_montserrat_16,
                                  0x9AA4AF);
    lv_obj_set_width(battery_base_label, landscape ? left_w - 32 : ui_inner_width());
    lv_label_set_long_mode(battery_base_label, LV_LABEL_LONG_DOT);
    lv_obj_align(battery_base_label, LV_ALIGN_TOP_LEFT, 0, 48);

    battery_status_label = ui_label(summary, "--", &lv_font_montserrat_20,
                                    0x9AA4AF);
    lv_obj_set_width(battery_status_label,
                     landscape ? left_w - 32 : ui_inner_width());
    lv_label_set_long_mode(battery_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(battery_status_label, LV_ALIGN_TOP_LEFT, 0, 86);

    battery_soc_label = ui_label(summary, "--", &lv_font_montserrat_48, 0xA3E635);
    lv_obj_align(battery_soc_label, LV_ALIGN_TOP_LEFT, 0, 138);
    ui_label(summary, "State of charge", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(lv_obj_get_child(summary, lv_obj_get_child_count(summary) - 1),
                 LV_ALIGN_TOP_RIGHT, 0, 166);

    battery_soc_bar = lv_bar_create(summary);
    lv_obj_set_pos(battery_soc_bar, 0, 232);
    lv_obj_set_size(battery_soc_bar,
                    landscape ? left_w - 32 :
                    ui_fit_width(lv_obj_get_parent(battery_soc_bar), 0, 488),
                    26);
    lv_bar_set_range(battery_soc_bar, 0, 100);
    lv_bar_set_value(battery_soc_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battery_soc_bar, lv_color_hex(0x2A3037),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_soc_bar, lv_color_hex(0xA3E635),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(battery_soc_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(battery_soc_bar, 8, LV_PART_INDICATOR);

    battery_info_row(metrics_a, landscape ? 16 : 310, "Voltage", "--", 0xA3E635);
    battery_voltage_label = lv_obj_get_child(metrics_a,
                                             lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 70 : 364, "Current", "--", 0x3DA5FF);
    battery_current_label = lv_obj_get_child(metrics_a,
                                             lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 124 : 418, "Remaining", "--", 0x25C281);
    battery_remaining_label = lv_obj_get_child(metrics_a,
                                               lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 178 : 472, "Full capacity", "--", 0x25C281);
    battery_full_label = lv_obj_get_child(metrics_a,
                                          lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 232 : 526, "Design capacity", "--", 0x9AA4AF);
    battery_design_label = lv_obj_get_child(metrics_a,
                                            lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 286 : 580, "Temperature", "--", 0xF5A524);
    battery_temp_label = lv_obj_get_child(metrics_a,
                                          lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 340 : 634, "Internal temp", "--",
                     0xF5A524);
    battery_internal_temp_label = lv_obj_get_child(
        metrics_a, lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 394 : 688, "Time to empty", "--",
                     0xF2F5F8);
    battery_time_empty_label = lv_obj_get_child(
        metrics_a, lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 448 : 742, "Time to full", "--",
                     0xF2F5F8);
    battery_time_full_label = lv_obj_get_child(
        metrics_a, lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 502 : 796, "State of health", "--",
                     0xA3E635);
    battery_health_label = lv_obj_get_child(metrics_a,
                                            lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 556 : 850, "Cycle count", "--", 0x9AA4AF);
    battery_cycle_label = lv_obj_get_child(metrics_a,
                                           lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 610 : 904, "Average power", "--", 0x22D3EE);
    battery_power_label = lv_obj_get_child(metrics_a,
                                           lv_obj_get_child_count(metrics_a) - 1);
    battery_info_row(metrics_a, landscape ? 664 : 958, "Status", "--", 0xEF4D5A);
    battery_status_reg_label = lv_obj_get_child(
        metrics_a, lv_obj_get_child_count(metrics_a) - 1);

    hardware_page_timer = lv_timer_create(battery_timer_cb, 1500, NULL);
    battery_update_page();
}

void ui_keyboard_test_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *status_panel;
    lv_obj_t *keys_panel;
    lv_obj_t *clear;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(154);
    int body_w = ui_screen_width() - 48;
    int status_w = landscape ? 280 : 520;
    int keys_x = landscape ? status_w + 24 : 0;
    int keys_w = landscape ? body_w - keys_x : 520;
    int keys_inner_w = landscape ? keys_w - 32 : 520;
    int gap = landscape ? 3 : 4;
    int key_w;
    int key_h = 50;
    int grid_y = landscape ? 0 : 280;
    int grid_h;
    int backlight_row_y;
    int backlight_slider_y;

    if(keys_inner_w < 260) {
        keys_inner_w = 260;
    }
    if(landscape) {
        key_h = (body_h - 96 - gap * (KEYBOARD_LAYOUT_ROWS - 1)) /
                KEYBOARD_LAYOUT_ROWS;
        key_h = clamp_int(key_h, 34, 56);
    }
    key_w = landscape ? (keys_inner_w - gap * (KEYBOARD_LAYOUT_COLS - 1)) /
                        KEYBOARD_LAYOUT_COLS : 40;
    if(key_w < 28) {
        key_w = 28;
    }
    grid_h = KEYBOARD_LAYOUT_ROWS * key_h + (KEYBOARD_LAYOUT_ROWS - 1) * gap;
    backlight_row_y = landscape ? grid_y + grid_h + 12 : 725;
    backlight_slider_y = backlight_row_y + 48;

    ui_create_header(scr, "Keyboard");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154),
                           landscape ? body_w : 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    if(landscape) {
        status_panel = ui_panel(body, 0, 0, status_w, body_h);
        keys_panel = ui_panel(body, keys_x, 0, keys_w, body_h);
    } else {
        status_panel = body;
        keys_panel = body;
    }

    ui_label(status_panel, "TCA8418", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(status_panel, lv_obj_get_child_count(status_panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    keyboard_base_label = ui_label(status_panel, "--", &lv_font_montserrat_16,
                                   0x9AA4AF);
    lv_obj_set_width(keyboard_base_label,
                     landscape ? status_w - 32 : ui_inner_width());
    lv_label_set_long_mode(keyboard_base_label, LV_LABEL_LONG_DOT);
    lv_obj_align(keyboard_base_label, LV_ALIGN_TOP_LEFT, 0, 48);

    keyboard_status_label = ui_label(status_panel, "--",
                                     landscape ? &lv_font_montserrat_18 :
                                     &lv_font_montserrat_20,
                                     0x9AA4AF);
    lv_obj_set_width(keyboard_status_label,
                     landscape ? status_w - 32 : ui_inner_width());
    lv_label_set_long_mode(keyboard_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(keyboard_status_label, LV_ALIGN_TOP_LEFT, 0, 86);

    if(landscape) {
        ui_label(status_panel, "Last key", &lv_font_montserrat_16,
                 0x9AA4AF);
        lv_obj_align(lv_obj_get_child(status_panel,
                     lv_obj_get_child_count(status_panel) - 1),
                     LV_ALIGN_TOP_LEFT, 0, 142);
        keyboard_last_label = ui_label(status_panel, "No event",
                                       &lv_font_montserrat_18, 0xF97316);
        lv_obj_set_width(keyboard_last_label, status_w - 32);
        lv_label_set_long_mode(keyboard_last_label, LV_LABEL_LONG_DOT);
        lv_obj_align(keyboard_last_label, LV_ALIGN_TOP_LEFT, 0, 166);

        ui_label(status_panel, "Event count", &lv_font_montserrat_16,
                 0x9AA4AF);
        lv_obj_align(lv_obj_get_child(status_panel,
                     lv_obj_get_child_count(status_panel) - 1),
                     LV_ALIGN_TOP_LEFT, 0, 218);
        keyboard_count_label = ui_label(status_panel, "0",
                                        &lv_font_montserrat_16, 0xA3E635);
        lv_obj_set_width(keyboard_count_label, status_w - 32);
        lv_label_set_long_mode(keyboard_count_label, LV_LABEL_LONG_DOT);
        lv_obj_align(keyboard_count_label, LV_ALIGN_TOP_LEFT, 0, 242);
    } else {
        ui_info_row(status_panel, 146, "Last key", "No event", 0xF97316);
        keyboard_last_label = lv_obj_get_child(status_panel,
                                               lv_obj_get_child_count(status_panel) - 1);
        ui_info_row(status_panel, 200, "Event count", "0", 0xA3E635);
        keyboard_count_label = lv_obj_get_child(status_panel,
                                                lv_obj_get_child_count(status_panel) - 1);
    }

    for(int row = 0; row < KEYBOARD_LAYOUT_ROWS; row++) {
        for(int col = 0; col < KEYBOARD_LAYOUT_COLS; col++) {
            int code = keyboard_display_layout[row][col];
            const lv_font_t *font = landscape ? &lv_font_montserrat_14 :
                                     &lv_font_montserrat_14;
            char key_text[16];
            lv_obj_t *box;

            if(code == 0) {
                continue;
            }

            tca8418_key_grid_text(code, landscape, key_text,
                                  sizeof(key_text));
            if(strlen(key_text) >= 4 || strchr(key_text, '\n')) {
                font = &lv_font_montserrat_12;
            }

            box = lv_obj_create(keys_panel);
            lv_obj_set_pos(box, col * (key_w + gap),
                           grid_y + row * (key_h + gap));
            lv_obj_set_size(box, key_w, key_h);
            lv_obj_set_style_radius(box, 6, 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(box, lv_color_hex(0x202832), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0x2A3037), 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            keyboard_key_box[row][col] = box;

            keyboard_key_label[row][col] = ui_label(box, key_text, font,
                                                    0x9AA4AF);
            lv_obj_set_width(keyboard_key_label[row][col], key_w - 2);
            lv_label_set_long_mode(keyboard_key_label[row][col],
                                   LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(keyboard_key_label[row][col],
                                        LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(keyboard_key_label[row][col]);
        }
    }

    clear = ui_command_button(status_panel, 0, landscape ? 340 : 640,
                              landscape ? status_w - 32 : 488,
                              "Clear keys", 0xF97316);
    lv_obj_add_event_cb(clear, keyboard_clear_event_cb, LV_EVENT_CLICKED, NULL);

    ui_info_row(keys_panel, backlight_row_y, "Keyboard backlight", "--",
                0x22D3EE);
    keyboard_backlight_label = lv_obj_get_child(
        keys_panel, lv_obj_get_child_count(keys_panel) - 1);
    keyboard_backlight_slider = lv_slider_create(keys_panel);
    lv_obj_set_pos(keyboard_backlight_slider, 0, backlight_slider_y);
    lv_obj_set_size(keyboard_backlight_slider,
                    landscape ? keys_inner_w :
                    ui_fit_width(lv_obj_get_parent(keyboard_backlight_slider), 0, 488),
                    28);
    lv_slider_set_range(keyboard_backlight_slider, 0, 100);
    lv_obj_set_style_bg_color(keyboard_backlight_slider,
                              lv_color_hex(0x202832), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard_backlight_slider,
                              lv_color_hex(0x22D3EE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(keyboard_backlight_slider,
                              lv_color_hex(0xF2F5F8), LV_PART_KNOB);
    lv_obj_set_style_height(keyboard_backlight_slider,
                            landscape ? 16 : 22, LV_PART_MAIN);
    lv_obj_set_style_width(keyboard_backlight_slider,
                           landscape ? 6 : 12, LV_PART_KNOB);
    lv_obj_set_style_height(keyboard_backlight_slider,
                            landscape ? 6 : 12, LV_PART_KNOB);
    lv_obj_add_event_cb(keyboard_backlight_slider, keyboard_backlight_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(keyboard_backlight_slider,
                        keyboard_backlight_slider_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_remove_flag(keyboard_backlight_slider,
                       LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(keyboard_backlight_slider);

    pthread_mutex_lock(&tca8418_event_lock);
    memset(keyboard_key_pressed, 0, sizeof(keyboard_key_pressed));
    keyboard_event_total = 0;
    tca8418_last_fifo_count = 0;
    snprintf(keyboard_last_event_text, sizeof(keyboard_last_event_text),
             "No event");
    pthread_mutex_unlock(&tca8418_event_lock);
    tca8418_raw_queue_clear();
    if(!keyboard_tca8418_ready) {
        keyboard_last_init_us = 0;
        tca8418_init_device();
    }
    tca8418_irq_start();
    keyboard_test_sound_enabled = 1;
    keyboard_last_click_us = 0;

    keyboard_backlight_update_ui();
    hardware_page_timer = lv_timer_create(keyboard_timer_cb, 120, NULL);
    keyboard_update_page();
}

static void button_update_one(unsigned int gpio, const char *name,
                              int active_high, int idle_value,
                              lv_obj_t *state_label, lv_obj_t *raw_label)
{
    static int last_value[2] = {-999, -999};
    static int last_failed[2];
    static uint64_t last_log_us[2];
    int value = -1;
    int active_level;
    int log_index = button_log_index(gpio);
    uint64_t now = ui_monotonic_us();
    char text[128];
    char source[40];
    char err[384];

    if(!state_label || !raw_label) {
        return;
    }

    if(button_read_gpio(gpio, &value, source, sizeof(source), err,
                        sizeof(err)) != 0) {
        snprintf(text, sizeof(text), "%s unavailable", name);
        lv_label_set_text(state_label, text);
        lv_obj_set_style_text_color(state_label, lv_color_hex(0xEF4D5A), 0);
        lv_label_set_text(raw_label, err[0] ? err : "--");
        if(log_index < 0 || !last_failed[log_index] ||
           now - last_log_us[log_index] > 2000000ULL) {
            button_test_log("%s GPIO%u read failed: %s", name, gpio,
                            err[0] ? err : "unknown");
            if(log_index >= 0) {
                last_log_us[log_index] = now;
            }
        }
        if(log_index >= 0) {
            last_failed[log_index] = 1;
            last_value[log_index] = -999;
        }
        return;
    }

    if(log_index >= 0) {
        last_failed[log_index] = 0;
    }
    active_level = active_high ? value != 0 : value == 0;
    lv_label_set_text(state_label, value == idle_value ? "Released" :
                      "Active level");
    lv_obj_set_style_text_color(state_label,
                                lv_color_hex(value == idle_value ? 0x9AA4AF :
                                             0xF5A524),
                                0);
    snprintf(text, sizeof(text), "GPIO%u raw=%d idle=%d press=%s %s%s",
             gpio, value, idle_value, active_high ? "high" : "low",
             source, active_level ? " active" : "");
    lv_label_set_text(raw_label, text);
    if(value != idle_value &&
       (log_index < 0 || last_value[log_index] != value ||
        now - last_log_us[log_index] > 2000000ULL)) {
        button_test_log("%s GPIO%u raw=%d differs from idle=%d source=%s",
                        name, gpio, value, idle_value, source);
        if(log_index >= 0) {
            last_log_us[log_index] = now;
        }
    }
    if(log_index >= 0) {
        last_value[log_index] = value;
    }
}

static void button_test_update_page(void)
{
    if(!button_status_label) {
        return;
    }

    button_update_one(BUTTON_BOOT0_GPIO, "BOOT0", 0, BUTTON_BOOT0_IDLE_VALUE,
                      button_boot0_state_label, button_boot0_raw_label);
#if 0
    button_update_one(BUTTON_INT0_GPIO, "INT0", 1, BUTTON_INT0_IDLE_VALUE,
                      button_int0_state_label, button_int0_raw_label);
#endif
    lv_label_set_text(button_status_label,
                      "Polling every 200 ms; raw level diagnostics");
}

static void button_test_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    button_test_update_page();
}

static lv_obj_t *button_state_card(lv_obj_t *parent, int x, int y, int w,
                                   const char *title, unsigned int gpio,
                                   lv_obj_t **state_out, lv_obj_t **raw_out)
{
    lv_obj_t *card = ui_panel(parent, x, y, w, 184);
    lv_obj_t *name;
    lv_obj_t *gpio_label;
    lv_obj_t *state;
    lv_obj_t *raw;
    char text[32];

    lv_obj_set_style_bg_color(card, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263544), 0);

    name = ui_label(card, title, &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(name, 0, 0);

    snprintf(text, sizeof(text), "GPIO%u", gpio);
    gpio_label = ui_label(card, text, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(gpio_label, LV_ALIGN_TOP_RIGHT, 0, 4);

    state = ui_label(card, "--", &lv_font_montserrat_32, 0x9AA4AF);
    lv_obj_set_pos(state, 0, 54);

    raw = ui_label(card, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(raw, w - 32);
    lv_label_set_long_mode(raw, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(raw, 0, 114);

    if(state_out) {
        *state_out = state;
    }
    if(raw_out) {
        *raw_out = raw;
    }
    return card;
}

void ui_button_test_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *hint;
    int landscape = ui_is_landscape();
    int body_w = ui_screen_width() - 48;
    int card_gap = 20;
    int card_w = landscape ? (body_w - card_gap) / 2 : 520;

    if(card_w < 260) {
        card_w = 260;
    }

    button_prepare_boot0_gpio();

    ui_create_header(scr, "Buttons");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    button_status_label = ui_label(body, "Ready", &lv_font_montserrat_18,
                                   0x9AA4AF);
    lv_obj_set_width(button_status_label, body_w);
    lv_label_set_long_mode(button_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(button_status_label, 0, 0);

    hint = ui_label(body, "BOOT0 idle=high/press=low. Details: /tmp/k230_button_test.log",
                    &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(hint, body_w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hint, 0, 34);

    button_state_card(body, 0, 82, card_w, "BOOT0", BUTTON_BOOT0_GPIO,
                      &button_boot0_state_label, &button_boot0_raw_label);
#if 0
    button_state_card(body, landscape ? card_w + card_gap : 0,
                      landscape ? 82 : 290, card_w, "INT0",
                      BUTTON_INT0_GPIO, &button_int0_state_label,
                      &button_int0_raw_label);
#endif

    hardware_page_timer = lv_timer_create(button_test_timer_cb, 200, NULL);
    button_test_update_page();
}

static int int0_test_last_value = -1;
static int int0_test_waiting_release;
static int int0_test_configured;
static unsigned int int0_test_press_count;
static unsigned int int0_test_release_count;
static unsigned int int0_test_edge_count;
static unsigned int int0_test_short_count;
static unsigned int int0_test_long_count;
static unsigned int int0_test_shutdown_count;
static uint64_t int0_test_last_change_us;
static volatile uint8_t *int0_pmu_regs;
static volatile uint8_t *int0_pwr_regs;

static void int0_pmu_unmap(void)
{
    if(int0_pmu_regs) {
        munmap((void *)int0_pmu_regs, INT0_PMU_IO_SIZE);
        int0_pmu_regs = NULL;
    }
    if(int0_pwr_regs) {
        munmap((void *)int0_pwr_regs, INT0_PWR_IO_SIZE);
        int0_pwr_regs = NULL;
    }
    int0_test_configured = 0;
}

static int int0_pmu_map(char *err, size_t err_len)
{
    int fd;

    if(int0_pmu_regs && int0_pwr_regs) {
        return 0;
    }

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "open /dev/mem failed: %s",
                     strerror(errno));
        }
        return -1;
    }

    if(!int0_pmu_regs) {
        int0_pmu_regs = mmap(NULL, INT0_PMU_IO_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, INT0_PMU_BASE_ADDR);
        if(int0_pmu_regs == MAP_FAILED) {
            int0_pmu_regs = NULL;
            if(err && err_len > 0) {
                snprintf(err, err_len, "mmap PMU 0x%08lx failed: %s",
                         INT0_PMU_BASE_ADDR, strerror(errno));
            }
            close(fd);
            return -1;
        }
    }

    if(!int0_pwr_regs) {
        int0_pwr_regs = mmap(NULL, INT0_PWR_IO_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, INT0_PWR_BASE_ADDR);
        if(int0_pwr_regs == MAP_FAILED) {
            int0_pwr_regs = NULL;
            if(err && err_len > 0) {
                snprintf(err, err_len, "mmap PWR 0x%08lx failed: %s",
                         INT0_PWR_BASE_ADDR, strerror(errno));
            }
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static uint32_t int0_pmu_read(uint32_t reg)
{
    return *(volatile uint32_t *)(int0_pmu_regs + reg);
}

static void int0_pmu_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(int0_pmu_regs + reg) = value;
}

static uint32_t int0_pwr_read(uint32_t reg)
{
    return *(volatile uint32_t *)(int0_pwr_regs + reg);
}

static void int0_pwr_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(int0_pwr_regs + reg) = value;
}

static void int0_pmu_set_edge(int falling)
{
    uint32_t value;
    uint32_t edge;

    value = int0_pmu_read(INT0_PMU_INT_DETECT_TYP);
    value &= ~(INT0_PMU_INT_TRIGGER_MASK << INT0_PMU_KEY_EDGE_OFFSET);
    edge = falling ? (INT0_PMU_INT_TRIGGER_TYPE_MASK |
                      INT0_PMU_INT_TRIGGER_EDGE_MASK) :
                     INT0_PMU_INT_TRIGGER_TYPE_MASK;
    value |= edge << INT0_PMU_KEY_EDGE_OFFSET;
    int0_pmu_write(INT0_PMU_INT_DETECT_TYP, value);
}

static void int0_pmu_clear(uint32_t status)
{
    uint32_t clear = 0;

    if(status & INT0_PMU_IRQ_KEY_LONG) {
        clear |= INT0_PMU_CLR_KEY_LONG;
    }
    if(status & INT0_PMU_IRQ_KEY_SHORT) {
        clear |= INT0_PMU_CLR_KEY_SHORT;
    }
    if(status & INT0_PMU_IRQ_KEY_EDGE) {
        clear |= INT0_PMU_CLR_KEY_EDGE;
    }
    if(status & INT0_PMU_IRQ_KEY_SHUTDOWN) {
        clear |= INT0_PMU_CLR_KEY_SHUTDOWN;
    }
    if(clear) {
        int0_pmu_write(INT0_PMU_INT_DETECT_CLR, clear);
    }
}

static int int0_pmu_configure(char *err, size_t err_len)
{
    uint32_t value;

    if(int0_pmu_map(err, err_len) != 0) {
        return -1;
    }

    value = int0_pwr_read(INT0_PMU_PWR_ISO_CTRL_REG);
    if(value & INT0_PMU_ISO_ACCESS_MASK) {
        int0_pwr_write(INT0_PMU_PWR_ISO_CTRL_REG,
                       value & ~INT0_PMU_ISO_ACCESS_MASK);
    }

    int0_pmu_write(INT0_PMU_INT0_LONG_PRESS_TRIGGER_VAL,
                   INT0_PMU_PWRKEY_LONG_PRESS_TICKS);
    int0_pmu_write(INT0_PMU_INT0_LEVEL_DEBOUNCE_VAL,
                   INT0_PMU_PWRKEY_DEBOUNCE_TICKS);

    value = int0_pmu_read(INT0_PMU_INT0_TO_CPU_REGISTER);
    value &= ~INT0_PMU_CPU_IRQ_MASK;
    int0_pmu_write(INT0_PMU_INT0_TO_CPU_REGISTER, value);

    value = int0_pmu_read(INT0_PMU_INT_DETECT_EN);
    value |= INT0_PMU_DET_KEY_EDGE | INT0_PMU_DET_KEY_SHORT |
             INT0_PMU_DET_KEY_LONG;
    int0_pmu_write(INT0_PMU_INT_DETECT_EN, value);

    int0_pmu_clear(INT0_PMU_IRQ_KEY_LONG | INT0_PMU_IRQ_KEY_SHORT |
                   INT0_PMU_IRQ_KEY_EDGE | INT0_PMU_IRQ_KEY_SHUTDOWN);
    int0_pmu_set_edge(0);
    int0_test_waiting_release = 0;
    int0_test_configured = 1;
    button_test_log("INT0 PMU pwrkey test configured en=0x%08x typ=0x%08x",
                    int0_pmu_read(INT0_PMU_INT_DETECT_EN),
                    int0_pmu_read(INT0_PMU_INT_DETECT_TYP));
    return 0;
}

static void int0_test_reset_counters(void)
{
    int0_test_last_value = -1;
    int0_test_waiting_release = 0;
    int0_test_press_count = 0;
    int0_test_release_count = 0;
    int0_test_edge_count = 0;
    int0_test_short_count = 0;
    int0_test_long_count = 0;
    int0_test_shutdown_count = 0;
    int0_test_last_change_us = 0;
    button_test_log("INT0 test counters reset");
    if(int0_pmu_regs) {
        int0_pmu_clear(INT0_PMU_IRQ_KEY_LONG | INT0_PMU_IRQ_KEY_SHORT |
                       INT0_PMU_IRQ_KEY_EDGE | INT0_PMU_IRQ_KEY_SHUTDOWN);
        int0_pmu_set_edge(0);
    }
}

static void int0_test_update_page(void)
{
    uint32_t raw_status;
    uint32_t status;
    uint32_t detect_en;
    uint32_t detect_typ;
    uint32_t cpu_route;
    int mux_value = -1;
    uint64_t now = ui_monotonic_us();
    char source[64];
    char err[256];
    char text[256];

    if(!int0_test_state_label) {
        return;
    }

    source[0] = '\0';
    err[0] = '\0';
    if(!int0_test_configured &&
       int0_pmu_configure(err, sizeof(err)) != 0) {
        lv_label_set_text(int0_test_state_label, "PMU unavailable");
        lv_obj_set_style_text_color(int0_test_state_label,
                                    lv_color_hex(0xEF4D5A), 0);
        lv_label_set_text(int0_test_raw_label, err[0] ? err : "unknown");
        snprintf(text, sizeof(text),
                 "press=%u release=%u edge=%u short=%u long=%u",
                 int0_test_press_count, int0_test_release_count,
                 int0_test_edge_count, int0_test_short_count,
                 int0_test_long_count);
        lv_label_set_text(int0_test_counts_label, text);
        lv_label_set_text(int0_test_log_label,
                          "PMU register map failed; check /dev/mem permission");
        return;
    }

    raw_status = int0_pmu_read(INT0_PMU_INT_STATE_REG);
    detect_en = int0_pmu_read(INT0_PMU_INT_DETECT_EN);
    detect_typ = int0_pmu_read(INT0_PMU_INT_DETECT_TYP);
    cpu_route = int0_pmu_read(INT0_PMU_INT0_TO_CPU_REGISTER);
    status = raw_status & (INT0_PMU_IRQ_KEY_LONG |
                           INT0_PMU_IRQ_KEY_SHORT |
                           INT0_PMU_IRQ_KEY_EDGE |
                           INT0_PMU_IRQ_KEY_SHUTDOWN);

    if(status & INT0_PMU_IRQ_KEY_EDGE) {
        int0_test_edge_count++;
        if(!int0_test_waiting_release) {
            int0_test_press_count++;
            int0_test_last_value = 1;
            int0_test_waiting_release = 1;
            int0_pmu_set_edge(1);
            button_test_log("INT0 PMU edge press status=0x%08x", raw_status);
        } else {
            int0_test_release_count++;
            int0_test_last_value = 0;
            int0_test_waiting_release = 0;
            int0_pmu_set_edge(0);
            button_test_log("INT0 PMU edge release status=0x%08x",
                            raw_status);
        }
        int0_test_last_change_us = now;
    }
    if(status & INT0_PMU_IRQ_KEY_SHORT) {
        int0_test_short_count++;
        button_test_log("INT0 PMU short status=0x%08x", raw_status);
    }
    if(status & INT0_PMU_IRQ_KEY_LONG) {
        int0_test_long_count++;
        button_test_log("INT0 PMU long status=0x%08x", raw_status);
    }
    if(status & INT0_PMU_IRQ_KEY_SHUTDOWN) {
        int0_test_shutdown_count++;
        button_test_log("INT0 PMU shutdown status=0x%08x", raw_status);
    }
    if(status) {
        int0_pmu_clear(status);
        detect_typ = int0_pmu_read(INT0_PMU_INT_DETECT_TYP);
    }

    lv_label_set_text(int0_test_state_label,
                      int0_test_waiting_release ? "Pressed / wait release" :
                      "Released / wait press");
    lv_obj_set_style_text_color(int0_test_state_label,
                                lv_color_hex(int0_test_waiting_release ?
                                             0xF5A524 : 0x9AA4AF),
                                0);
    (void)button_read_int0_pmuiomux(&mux_value, source, sizeof(source),
                                    NULL, 0);
    snprintf(text, sizeof(text),
             "pmu_status=0x%08x key=0x%08x en=0x%08x typ=0x%08x route=0x%08x %s",
             raw_status, status, detect_en, detect_typ, cpu_route, source);
    lv_label_set_text(int0_test_raw_label, text);

    if(int0_test_last_change_us > 0) {
        snprintf(text, sizeof(text),
                 "press=%u release=%u edge=%u short=%u long=%u last=%.2fs",
                 int0_test_press_count, int0_test_release_count,
                 int0_test_edge_count, int0_test_short_count,
                 int0_test_long_count,
                 (double)(now - int0_test_last_change_us) / 1000000.0);
    } else {
        snprintf(text, sizeof(text),
                 "press=%u release=%u edge=%u short=%u long=%u shutdown=%u",
                 int0_test_press_count, int0_test_release_count,
                 int0_test_edge_count, int0_test_short_count,
                 int0_test_long_count, int0_test_shutdown_count);
    }
    lv_label_set_text(int0_test_counts_label, text);
    lv_label_set_text(int0_test_log_label,
                      "RTOS PMU pwrkey path: edge/short/long event poll. Log: "
                      BUTTON_TEST_LOG);
}

static void int0_test_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int0_test_update_page();
}

static void int0_test_reset_event_cb(lv_event_t *event)
{
    (void)event;
    int0_test_reset_counters();
    int0_test_update_page();
}

void ui_int0_test_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *hint;
    int landscape = ui_is_landscape();
    int body_w = ui_fit_width(scr, 24, 520);
    int card_w = landscape ? body_w : 520;

    if(card_w < 260) {
        card_w = 260;
    }

    ui_create_header(scr, "INT0 Test");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), body_w,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    hint = ui_label(body,
                    "PMU pwrkey register event test",
                    &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(hint, body_w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hint, 0, 0);

    button_state_card(body, 0, 54, card_w, "INT0 / PMU IO0",
                      BUTTON_INT0_GPIO, &int0_test_state_label,
                      &int0_test_raw_label);

    int0_test_counts_label = ui_label(body, "--", &lv_font_montserrat_20,
                                      0xF2F5F8);
    lv_obj_set_width(int0_test_counts_label, body_w);
    lv_label_set_long_mode(int0_test_counts_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(int0_test_counts_label, 0, 264);

    int0_test_log_label = ui_label(body, "--", &lv_font_montserrat_16,
                                   0x9AA4AF);
    lv_obj_set_width(int0_test_log_label, body_w);
    lv_label_set_long_mode(int0_test_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(int0_test_log_label, 0, 304);

    int0_test_reset_btn = ui_command_button(body, 0, 374,
                                            ui_fit_width(body, 0, 488),
                                            "Reset counters", 0xF97316);
    lv_obj_add_event_cb(int0_test_reset_btn, int0_test_reset_event_cb,
                        LV_EVENT_CLICKED, NULL);

    int0_test_reset_counters();
    hardware_page_timer = lv_timer_create(int0_test_timer_cb, 80, NULL);
    int0_test_update_page();
}

void ui_xl9555_led_create(lv_obj_t *scr)
{
    static const char *labels[3] = {
        "LED P03",
        "LED P04",
        "LED P05"
    };
    lv_obj_t *body;
    lv_obj_t *btn;

    ui_create_header(scr, "LED Test");
    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    ui_label(body, "XL9555", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    xl9555_base_label = ui_label(body, "--", &lv_font_montserrat_16,
                                 0x9AA4AF);
    lv_obj_set_width(xl9555_base_label, ui_inner_width());
    lv_label_set_long_mode(xl9555_base_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xl9555_base_label, LV_ALIGN_TOP_LEFT, 0, 48);

    xl9555_status_label = ui_label(body, "--", &lv_font_montserrat_20,
                                   0x9AA4AF);
    lv_obj_set_width(xl9555_status_label, ui_inner_width());
    lv_label_set_long_mode(xl9555_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xl9555_status_label, LV_ALIGN_TOP_LEFT, 0, 86);

    ui_info_row(body, 148, "Output register", "--", 0x22D3EE);
    xl9555_output_label = lv_obj_get_child(body,
                                           lv_obj_get_child_count(body) - 1);
    ui_info_row(body, 202, "Direction register", "--", 0xF5A524);
    xl9555_config_label = lv_obj_get_child(body,
                                           lv_obj_get_child_count(body) - 1);
    ui_info_row(body, 256, "I2C", "I2C4 SDA47/SCL46 addr=0x20", 0xF2F5F8);
    ui_info_row(body, 310, "Pins", "P03 P04 P05 output low=on", 0x9AA4AF);

    for(size_t i = 0; i < 3; i++) {
        xl9555_led_btn[i] = ui_command_button(body, 0, 392 + (int)i * 78,
                                              488, labels[i], 0x22D3EE);
        lv_obj_add_event_cb(xl9555_led_btn[i], xl9555_led_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    btn = ui_command_button(body, 0, 638, 236, "All on", 0x25C281);
    lv_obj_add_event_cb(btn, xl9555_all_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);
    btn = ui_command_button(body, 252, 638, 236, "All off", 0xEF4D5A);
    lv_obj_add_event_cb(btn, xl9555_all_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)0);

    xl9555_ready = 0;
    hardware_page_timer = lv_timer_create(xl9555_timer_cb, 1000, NULL);
    xl9555_update_page();
}

void ui_hardware_cleanup(void)
{
    if(hardware_page_timer) {
        lv_timer_delete(hardware_page_timer);
        hardware_page_timer = NULL;
    }
    int0_pmu_unmap();

    audio_status_label = NULL;
    audio_headphones_btn = NULL;
    audio_external_btn = NULL;
    audio_settings_volume_slider = NULL;
    audio_settings_volume_label = NULL;
    fan_status_label = NULL;
    fan_temp_label = NULL;
    fan_mode_label = NULL;
    fan_duty_label = NULL;
    fan_on_label = NULL;
    fan_off_label = NULL;
    fan_off_btn = NULL;
    fan_on_btn = NULL;
    fan_auto_btn = NULL;
    fan_duty_slider = NULL;
    fan_on_slider = NULL;
    fan_off_slider = NULL;
    sensor_status_label = NULL;
    sensor_temp_label = NULL;
    sensor_humidity_label = NULL;
    sensor_bus_label = NULL;
    sensor_cpu_label = NULL;
    bq25896_status_label = NULL;
    bq25896_base_label = NULL;
    bq25896_charge_label = NULL;
    bq25896_slider_label = NULL;
    bq25896_input_label = NULL;
    bq25896_voltage_label = NULL;
    bq25896_vbat_label = NULL;
    bq25896_vsys_label = NULL;
    bq25896_vbus_label = NULL;
    bq25896_ichg_label = NULL;
    bq25896_ntc_label = NULL;
    bq25896_fault_label = NULL;
    bq25896_charge_on_btn = NULL;
    bq25896_charge_off_btn = NULL;
    bq25896_current_slider = NULL;
    battery_status_label = NULL;
    battery_base_label = NULL;
    battery_soc_label = NULL;
    battery_soc_bar = NULL;
    battery_voltage_label = NULL;
    battery_current_label = NULL;
    battery_remaining_label = NULL;
    battery_full_label = NULL;
    battery_design_label = NULL;
    battery_temp_label = NULL;
    battery_internal_temp_label = NULL;
    battery_time_empty_label = NULL;
    battery_time_full_label = NULL;
    battery_health_label = NULL;
    battery_cycle_label = NULL;
    battery_power_label = NULL;
    battery_status_reg_label = NULL;
    keyboard_status_label = NULL;
    keyboard_base_label = NULL;
    keyboard_last_label = NULL;
    keyboard_count_label = NULL;
    keyboard_backlight_label = NULL;
    keyboard_backlight_slider = NULL;
    keyboard_test_sound_enabled = 0;
    memset(keyboard_key_box, 0, sizeof(keyboard_key_box));
    memset(keyboard_key_label, 0, sizeof(keyboard_key_label));
    button_boot0_state_label = NULL;
    button_boot0_raw_label = NULL;
    button_int0_state_label = NULL;
    button_int0_raw_label = NULL;
    button_status_label = NULL;
    int0_test_state_label = NULL;
    int0_test_raw_label = NULL;
    int0_test_counts_label = NULL;
    int0_test_log_label = NULL;
    int0_test_reset_btn = NULL;
    xl9555_status_label = NULL;
    xl9555_base_label = NULL;
    xl9555_output_label = NULL;
    xl9555_config_label = NULL;
    memset(xl9555_led_btn, 0, sizeof(xl9555_led_btn));
}
