#include "ui_lora.h"

#include "ui_i18n.h"
#include "ui_input.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "modules/SX126x/SX1262.h"
#include "modules/SX126x/SX126x_commands.h"
#include "modules/LR2021/LR2021.h"
#include "protocols/LoRaWAN/LoRaWAN.h"

#define LORA_LOG_PATH "/tmp/k230_phone_lora.log"
#define LORA_SPI_DEV "/dev/spidev0.0"
#define LORA_PROFILE_STORE_PATH "/root/app/k230_phone_ui/lora_profiles.conf"
#define LORA_SPI_SPEED_HZ 4000000U
#define LORA_PIN_SCLK 15U
#define LORA_PIN_MOSI 16U
#define LORA_PIN_MISO 17U
#define LORA_PIN_CS 14U
#define LORA_PIN_RST 5U
#define LORA_PIN_BUSY 19U
#define LORA_PIN_DIO1 20U
#define LORA_LR2021_IRQ_DIO_NUM 11U
#define LORA_PIN_POWER 44U
#define LORA_MAX_LOG_LINES 8
#define LORA_LINE_MAX 160
#define LORA_MAX_MESSAGES 16
#define LORA_PROFILE_NAME_MAX 24
#define LORA_SUB1G_FACTORY_PROFILE_COUNT 5
#define LORA_LR2021_FACTORY_PROFILE_COUNT 7
#define LORA_DEFAULT_PROFILE_INDEX 2
#define LORA_LR2021_16E8_HF_POWER_DEFAULT 8
#define LORA_LR2021_16E8_HF_POWER_MAX 9
#define LORA_LR2021_BOOTSTRAP_FREQ 915.0f
#define LORA_LR2021_BOOTSTRAP_BW 125.0f
#define LORA_LR2021_BOOTSTRAP_SF 10
#define LORA_LR2021_BOOTSTRAP_CR 6
#define LORA_LR2021_BOOTSTRAP_SW 0x12
#define LORA_LR2021_BOOTSTRAP_PRE 15
#define LORA_SESSION_BUTTON_COUNT 3
#define LORA_MAX_PROFILES 16
#define LORA_UI_TICK_MS 50
#define LORA_STARTUP_DELAY_MS 160
#define LORA_AUTO_TX_DEFAULT 0
#define LORA_FIXED_PAYLOAD_DEFAULT "LilyGo LoRa Factory"
#define LORAWAN_CONFIG_DIR "/root/lorawan"
#define LORAWAN_CONFIG_PATH "/root/lorawan/otaa.conf"
#define LORAWAN_SESSION_PATH "/root/lorawan/session.bin"
#define LORAWAN_NONCES_PATH "/root/lorawan/nonces.bin"
#define LORAWAN_PAYLOAD_VERSION 1
#define LORAWAN_PROFILE_PREFIX "lora_wan_"
#define LORAWAN_PROFILE_EXT ".json"
#define LORAWAN_PROFILE_MAX 20
#define LORAWAN_FILE_MAX 96
#define LORAWAN_PATH_MAX 192
#define LORA_FLRC_LOG_PATH "/tmp/k230_lora_flrc.log"
#define LORA_FLRC_VIDEO_BIN "/root/app/k230_phone_ui/k230_lora_flrc_video"
#define LORA_FLRC_CAMERA_STREAM_BIN "/root/app/k230_phone_ui/k230_flrc_camera_stream.sh"
#define LORA_FLRC_VIDEO_LOG_PATH "/tmp/k230_lora_flrc_video_ui.log"
#define LORA_FLRC_VIDEO_FILE "/root/videos/video02.mp4"
#define LORA_FLRC_VIDEO_TX_DIR "/root/videos/flrc_tx"
#define LORA_FLRC_VIDEO_OUT_DIR "/root/videos/flrc_rx"
#define LORA_FLRC_PAYLOAD_LEN 252U
#define LORA_FLRC_CAMERA_STOP_FILE "/tmp/k230_flrc_camera_stream.stop"
#define LORA_FLRC_DEFAULT_FREQ 2400.0f
#define LORA_FLRC_DEFAULT_BR 2600U
#define LORA_FLRC_DEFAULT_POWER LORA_LR2021_16E8_HF_POWER_DEFAULT
#define LORA_FLRC_DEFAULT_SECONDS 15U
#define LORA_FLRC_RX_POLL_US 80U
#define LORA_FLRC_VIDEO_SPI_HZ 16000000U
#define LORA_FLRC_VIDEO_RX_POLL_US 50U
#define LORA_FLRC_VIDEO_RETRIES 10U
#define LORA_FLRC_VIDEO_ACK_WAIT_MS 5000U
#define LORA_FLRC_CAMERA_PREVIEW_FILE "/tmp/k230_flrc_camera_preview.rgb565"
#define LORA_FLRC_CAMERA_PREVIEW_META "/tmp/k230_flrc_camera_preview.meta"
#define LORA_FLRC_CAMERA_PREVIEW_W_DEFAULT 80U
#define LORA_FLRC_CAMERA_PREVIEW_H_DEFAULT 60U
#define LORA_FLRC_CAMERA_PREVIEW_W_MAX 160U
#define LORA_FLRC_CAMERA_PREVIEW_H_MAX 120U

typedef struct {
    const char *name;
    unsigned width;
    unsigned height;
    unsigned tile_w;
    unsigned tile_h;
    unsigned fps;
    unsigned jpeg_quality;
} lora_flrc_camera_preset_t;

static const lora_flrc_camera_preset_t lora_flrc_camera_presets[] = {
    {"Low 80x60", 80U, 60U, 20U, 5U, 4U, 32U},
    {"Balanced 120x90", 120U, 90U, 20U, 5U, 4U, 28U},
    {"Detail 160x120", 160U, 120U, 20U, 5U, 3U, 24U},
};

typedef enum {
    LORA_CHIP_NONE = 0,
    LORA_CHIP_SX1262,
    LORA_CHIP_LR2021,
} lora_chip_type_t;

typedef struct {
    char name[LORA_PROFILE_NAME_MAX];
    float freq;
    float bandwidth;
    int8_t power;
    uint8_t sf;
    uint8_t cr;
    uint8_t sync_word;
    uint16_t preamble;
    uint32_t interval_ms;
    int counter_payload;
    int builtin;
} lora_profile_t;

static const lora_profile_t lora_factory_profiles_sub1g[LORA_SUB1G_FACTORY_PROFILE_COUNT] = {
    {"Factory 433", 433.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 868", 868.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 915", 915.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 920", 920.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 923", 923.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
};

static const lora_profile_t lora_factory_profiles_lr2021[LORA_LR2021_FACTORY_PROFILE_COUNT] = {
    {"Factory 433", 433.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 868", 868.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 915", 915.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 920", 920.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 923", 923.0f, 125.0f, 22, 12, 5, 0xCD, 16, 1000, 1, 1},
    {"Factory 2400", 2400.0f, 125.0f, LORA_LR2021_16E8_HF_POWER_DEFAULT, 10, 6, 0x12, 15, 1000, 1, 1},
    {"Factory 2450", 2450.0f, 125.0f, LORA_LR2021_16E8_HF_POWER_DEFAULT, 10, 6, 0x12, 15, 1000, 1, 1},
};

static const uint32_t lora_lr2021_16e8_rf_switch_dio_pins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LR2021_DIO6, RADIOLIB_LR2021_DIO7, RADIOLIB_NC,
    RADIOLIB_NC, RADIOLIB_NC
};

static const Module::RfSwitchMode_t lora_lr2021_16e8_rf_switch_table[] = {
    { LR2021::MODE_STBY,  {0, 0, 0, 0, 0} },
    { LR2021::MODE_TX,    {0, 0, 0, 0, 0} },
    { LR2021::MODE_RX,    {0, 0, 0, 0, 0} },
    { LR2021::MODE_RX_HF, {1, 0, 0, 0, 0} },
    { LR2021::MODE_TX_HF, {0, 1, 0, 0, 0} },
    END_OF_MODE_TABLE,
};

static lora_profile_t lora_profiles[LORA_MAX_PROFILES];

typedef enum {
    LORA_SESSION_LISTEN = 0,
    LORA_SESSION_AUTO_TX,
    LORA_SESSION_CARRIER,
} lora_session_mode_t;

typedef enum {
    LORA_PAYLOAD_COUNTER = 0,
    LORA_PAYLOAD_FIXED,
} lora_payload_mode_t;

typedef enum {
    LORA_EDIT_NAME = 0,
    LORA_EDIT_FREQ,
    LORA_EDIT_BW,
    LORA_EDIT_POWER,
    LORA_EDIT_SF,
    LORA_EDIT_CR,
    LORA_EDIT_SYNC,
    LORA_EDIT_PREAMBLE,
    LORA_EDIT_INTERVAL,
    LORA_EDIT_FIELD_COUNT,
} lora_edit_field_t;

enum {
    K230_HAL_GPIO_INPUT = 0,
    K230_HAL_GPIO_OUTPUT = 1,
    K230_HAL_GPIO_LOW = 0,
    K230_HAL_GPIO_HIGH = 1,
    K230_HAL_GPIO_RISING = 1,
    K230_HAL_GPIO_FALLING = 2,
};

class K230LinuxHal : public RadioLibHal {
public:
    K230LinuxHal(const char *spi_path, uint32_t spi_speed)
        : RadioLibHal(K230_HAL_GPIO_INPUT, K230_HAL_GPIO_OUTPUT,
                      K230_HAL_GPIO_LOW, K230_HAL_GPIO_HIGH,
                      K230_HAL_GPIO_RISING, K230_HAL_GPIO_FALLING),
          spi_path_(spi_path), spi_speed_(spi_speed)
    {
        memset(gpio_lines_, 0, sizeof(gpio_lines_));
    }

    ~K230LinuxHal() override
    {
        term();
    }

    void init() override
    {
        spiBegin();
    }

    void term() override
    {
        stop_interrupt_thread();
        spiEnd();
        for(size_t i = 0; i < 64; i++) {
            release_pin((uint32_t)i);
        }
        for(size_t i = 0; i < 2; i++) {
            if(chips_[i]) {
                gpiod_chip_close(chips_[i]);
                chips_[i] = nullptr;
            }
        }
    }

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        if(pin == RADIOLIB_NC) {
            return;
        }
        request_pin(pin, mode == K230_HAL_GPIO_OUTPUT, K230_HAL_GPIO_LOW);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        if(pin == RADIOLIB_NC) {
            return;
        }
        if(!request_pin(pin, true, value)) {
            return;
        }
        enum gpiod_line_value line_value =
            value == K230_HAL_GPIO_HIGH ? GPIOD_LINE_VALUE_ACTIVE :
            GPIOD_LINE_VALUE_INACTIVE;
        if(gpiod_line_request_set_value(gpio_lines_[pin].request,
                                        pin_offset(pin), line_value) != 0) {
            set_error("gpio%u write failed: %s", pin, strerror(errno));
        }
    }

    uint32_t digitalRead(uint32_t pin) override
    {
        if(pin == RADIOLIB_NC) {
            return 0;
        }
        if(!request_pin(pin, false, K230_HAL_GPIO_LOW)) {
            return 0;
        }
        enum gpiod_line_value value =
            gpiod_line_request_get_value(gpio_lines_[pin].request,
                                         pin_offset(pin));
        if(value == GPIOD_LINE_VALUE_ERROR) {
            set_error("gpio%u read failed: %s", pin, strerror(errno));
            return 0;
        }
        return value == GPIOD_LINE_VALUE_ACTIVE ? K230_HAL_GPIO_HIGH :
               K230_HAL_GPIO_LOW;
    }

    void attachInterrupt(uint32_t interrupt_num, void (*interrupt_cb)(void),
                         uint32_t mode) override
    {
        if(interrupt_num == RADIOLIB_NC) {
            return;
        }
        if(interrupt_pin_ == interrupt_num && interrupt_thread_running_) {
            interrupt_cb_ = interrupt_cb;
            return;
        }
        stop_interrupt_thread();
        if(!request_interrupt_pin(interrupt_num, interrupt_cb, mode)) {
            return;
        }
    }

    void detachInterrupt(uint32_t interrupt_num) override
    {
        if(interrupt_num == RADIOLIB_NC || interrupt_num == interrupt_pin_) {
            stop_interrupt_thread();
        }
    }

    void delay(RadioLibTime_t ms) override
    {
        if(ms == 0) {
            sched_yield();
            return;
        }
        usleep((useconds_t)ms * 1000U);
    }

    void delayMicroseconds(RadioLibTime_t us) override
    {
        if(us == 0) {
            sched_yield();
            return;
        }
        usleep((useconds_t)us);
    }

    RadioLibTime_t millis() override
    {
        return micros() / 1000U;
    }

    RadioLibTime_t micros() override
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (RadioLibTime_t)((uint64_t)ts.tv_sec * 1000000ULL +
                                (uint64_t)ts.tv_nsec / 1000ULL);
    }

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override
    {
        RadioLibTime_t start = micros();
        while((micros() - start) < timeout) {
            if(digitalRead(pin) == state) {
                RadioLibTime_t pulse_start = micros();
                while((micros() - start) < timeout && digitalRead(pin) == state) {
                    delayMicroseconds(20);
                }
                return (long)(micros() - pulse_start);
            }
            delayMicroseconds(20);
        }
        return 0;
    }

    void spiBegin() override
    {
        uint8_t mode = SPI_MODE_0;
        uint8_t bits = 8;

        if(spi_fd_ >= 0) {
            return;
        }

        spi_fd_ = open(spi_path_, O_RDWR | O_CLOEXEC);
        if(spi_fd_ < 0) {
            set_error("%s missing: %s", spi_path_, strerror(errno));
            return;
        }

        if(ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
           ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
           ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed_) < 0) {
            set_error("%s configure failed: %s", spi_path_, strerror(errno));
            close(spi_fd_);
            spi_fd_ = -1;
        }
    }

    void spiBeginTransaction() override
    {
    }

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        struct spi_ioc_transfer tr;
        uint8_t *zero = nullptr;

        if(len == 0) {
            return;
        }
        if(spi_fd_ < 0) {
            spiBegin();
        }
        if(spi_fd_ < 0) {
            if(in) {
                memset(in, 0, len);
            }
            return;
        }

        if(!out) {
            zero = (uint8_t *)calloc(len, 1);
            out = zero;
        }

        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (uintptr_t)out;
        tr.rx_buf = (uintptr_t)in;
        tr.len = (uint32_t)len;
        tr.speed_hz = spi_speed_;
        tr.bits_per_word = 8;

        if(ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
            set_error("spi transfer failed: %s", strerror(errno));
            if(in) {
                memset(in, 0, len);
            }
        }

        free(zero);
    }

    void spiEndTransaction() override
    {
    }

    void spiEnd() override
    {
        if(spi_fd_ >= 0) {
            close(spi_fd_);
            spi_fd_ = -1;
        }
    }

    void yield() override
    {
        sched_yield();
    }

    const char *last_error() const
    {
        return last_error_;
    }

    int spi_ready() const
    {
        return spi_fd_ >= 0;
    }

private:
    typedef struct {
        struct gpiod_line_request *request;
        int output;
        int active;
        int edge;
    } gpio_line_t;

    const char *spi_path_;
    uint32_t spi_speed_;
    int spi_fd_ = -1;
    struct gpiod_chip *chips_[2] = {nullptr, nullptr};
    gpio_line_t gpio_lines_[64];
    char last_error_[160] = "";
    pthread_t interrupt_thread_ = 0;
    volatile int interrupt_thread_running_ = 0;
    int interrupt_pipe_[2] = {-1, -1};
    uint32_t interrupt_pin_ = RADIOLIB_NC;
    void (*interrupt_cb_)(void) = nullptr;

    static unsigned int pin_offset(uint32_t pin)
    {
        return pin < 32U ? pin : pin - 32U;
    }

    static unsigned int pin_chip(uint32_t pin)
    {
        return pin < 32U ? 0U : 1U;
    }

    void set_error(const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(last_error_, sizeof(last_error_), fmt, ap);
        va_end(ap);
    }

    void release_pin(uint32_t pin)
    {
        if(pin >= 64U || !gpio_lines_[pin].request) {
            return;
        }
        gpiod_line_request_release(gpio_lines_[pin].request);
        gpio_lines_[pin].request = nullptr;
        gpio_lines_[pin].active = 0;
        gpio_lines_[pin].edge = 0;
    }

    static void *interrupt_thread_entry(void *arg)
    {
        K230LinuxHal *self = (K230LinuxHal *)arg;
        self->interrupt_loop();
        return nullptr;
    }

    void interrupt_loop()
    {
        struct gpiod_edge_event_buffer *buffer =
            gpiod_edge_event_buffer_new(8);
        int gpio_fd;

        if(!buffer || interrupt_pin_ >= 64U ||
           !gpio_lines_[interrupt_pin_].request) {
            gpiod_edge_event_buffer_free(buffer);
            return;
        }

        gpio_fd = gpiod_line_request_get_fd(gpio_lines_[interrupt_pin_].request);
        while(interrupt_thread_running_) {
            struct pollfd fds[2];
            int rc;

            fds[0].fd = gpio_fd;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            fds[1].fd = interrupt_pipe_[0];
            fds[1].events = POLLIN;
            fds[1].revents = 0;

            rc = poll(fds, 2, -1);
            if(rc <= 0) {
                continue;
            }
            if(fds[1].revents & POLLIN) {
                break;
            }
            if(fds[0].revents & POLLIN) {
                int count = gpiod_line_request_read_edge_events(
                    gpio_lines_[interrupt_pin_].request, buffer, 8);
                if(count > 0 && interrupt_cb_) {
                    interrupt_cb_();
                }
            }
        }

        gpiod_edge_event_buffer_free(buffer);
    }

    void stop_interrupt_thread()
    {
        if(interrupt_thread_running_) {
            interrupt_thread_running_ = 0;
            if(interrupt_pipe_[1] >= 0) {
                const uint8_t byte = 1;
                ssize_t ignored = write(interrupt_pipe_[1], &byte, 1);
                (void)ignored;
            }
            pthread_join(interrupt_thread_, nullptr);
            interrupt_thread_ = 0;
        }

        if(interrupt_pipe_[0] >= 0) {
            close(interrupt_pipe_[0]);
            interrupt_pipe_[0] = -1;
        }
        if(interrupt_pipe_[1] >= 0) {
            close(interrupt_pipe_[1]);
            interrupt_pipe_[1] = -1;
        }

        if(interrupt_pin_ != RADIOLIB_NC && interrupt_pin_ < 64U) {
            release_pin(interrupt_pin_);
        }
        interrupt_pin_ = RADIOLIB_NC;
        interrupt_cb_ = nullptr;
    }

    struct gpiod_chip *open_chip(unsigned int chip)
    {
        const char *path = chip == 0U ? "/dev/gpiochip0" : "/dev/gpiochip1";

        if(chip > 1U) {
            return nullptr;
        }
        if(!chips_[chip]) {
            chips_[chip] = gpiod_chip_open(path);
            if(!chips_[chip]) {
                set_error("%s open failed: %s", path, strerror(errno));
            }
        }
        return chips_[chip];
    }

    int request_pin(uint32_t pin, bool output, uint32_t initial_value)
    {
        struct gpiod_chip *chip;
        struct gpiod_line_settings *settings;
        struct gpiod_line_config *line_config;
        struct gpiod_request_config *request_config;
        struct gpiod_line_request *request;
        unsigned int offset;
        unsigned int chip_index;

        if(pin >= 64U) {
            set_error("gpio%u out of range", pin);
            return 0;
        }

        if(gpio_lines_[pin].active && gpio_lines_[pin].output == (int)output) {
            return 1;
        }
        release_pin(pin);

        chip_index = pin_chip(pin);
        offset = pin_offset(pin);
        chip = open_chip(chip_index);
        if(!chip) {
            return 0;
        }

        settings = gpiod_line_settings_new();
        line_config = gpiod_line_config_new();
        request_config = gpiod_request_config_new();
        if(!settings || !line_config || !request_config) {
            set_error("gpio%u allocation failed", pin);
            gpiod_line_settings_free(settings);
            gpiod_line_config_free(line_config);
            gpiod_request_config_free(request_config);
            return 0;
        }

        gpiod_line_settings_set_direction(
            settings, output ? GPIOD_LINE_DIRECTION_OUTPUT :
            GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_AS_IS);
        if(output) {
            gpiod_line_settings_set_output_value(
                settings, initial_value == K230_HAL_GPIO_HIGH ?
                GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
        }
        gpiod_request_config_set_consumer(request_config, "k230-phone-lora");

        if(gpiod_line_config_add_line_settings(line_config, &offset, 1,
                                               settings) != 0) {
            set_error("gpio%u line config failed: %s", pin, strerror(errno));
            gpiod_line_settings_free(settings);
            gpiod_line_config_free(line_config);
            gpiod_request_config_free(request_config);
            return 0;
        }

        request = gpiod_chip_request_lines(chip, request_config, line_config);
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_config);
        gpiod_request_config_free(request_config);
        if(!request) {
            set_error("gpio%u request failed: %s", pin, strerror(errno));
            return 0;
        }

        gpio_lines_[pin].request = request;
        gpio_lines_[pin].output = output ? 1 : 0;
        gpio_lines_[pin].active = 1;
        gpio_lines_[pin].edge = 0;
        return 1;
    }

    int request_interrupt_pin(uint32_t pin, void (*callback)(void),
                              uint32_t mode)
    {
        struct gpiod_chip *chip;
        struct gpiod_line_settings *settings;
        struct gpiod_line_config *line_config;
        struct gpiod_request_config *request_config;
        struct gpiod_line_request *request;
        unsigned int offset;
        unsigned int chip_index;

        if(pin >= 64U) {
            set_error("gpio%u irq out of range", pin);
            return 0;
        }

        release_pin(pin);
        chip_index = pin_chip(pin);
        offset = pin_offset(pin);
        chip = open_chip(chip_index);
        if(!chip) {
            return 0;
        }

        settings = gpiod_line_settings_new();
        line_config = gpiod_line_config_new();
        request_config = gpiod_request_config_new();
        if(!settings || !line_config || !request_config) {
            set_error("gpio%u irq allocation failed", pin);
            gpiod_line_settings_free(settings);
            gpiod_line_config_free(line_config);
            gpiod_request_config_free(request_config);
            return 0;
        }

        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_AS_IS);
        gpiod_line_settings_set_edge_detection(
            settings, mode == K230_HAL_GPIO_FALLING ?
            GPIOD_LINE_EDGE_FALLING : GPIOD_LINE_EDGE_RISING);
        gpiod_request_config_set_consumer(request_config, "k230-phone-lora-irq");
        gpiod_request_config_set_event_buffer_size(request_config, 16);

        if(gpiod_line_config_add_line_settings(line_config, &offset, 1,
                                               settings) != 0) {
            set_error("gpio%u irq config failed: %s", pin, strerror(errno));
            gpiod_line_settings_free(settings);
            gpiod_line_config_free(line_config);
            gpiod_request_config_free(request_config);
            return 0;
        }

        request = gpiod_chip_request_lines(chip, request_config, line_config);
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_config);
        gpiod_request_config_free(request_config);
        if(!request) {
            set_error("gpio%u irq request failed: %s", pin, strerror(errno));
            return 0;
        }

        if(pipe(interrupt_pipe_) != 0) {
            set_error("gpio%u irq pipe failed: %s", pin, strerror(errno));
            gpiod_line_request_release(request);
            return 0;
        }

        gpio_lines_[pin].request = request;
        gpio_lines_[pin].output = 0;
        gpio_lines_[pin].active = 1;
        gpio_lines_[pin].edge = 1;
        interrupt_pin_ = pin;
        interrupt_cb_ = callback;
        interrupt_thread_running_ = 1;
        if(pthread_create(&interrupt_thread_, nullptr, interrupt_thread_entry,
                          this) != 0) {
            set_error("gpio%u irq thread failed", pin);
            interrupt_thread_running_ = 0;
            close(interrupt_pipe_[0]);
            close(interrupt_pipe_[1]);
            interrupt_pipe_[0] = -1;
            interrupt_pipe_[1] = -1;
            release_pin(pin);
            interrupt_pin_ = RADIOLIB_NC;
            interrupt_cb_ = nullptr;
            return 0;
        }
        return 1;
    }
};

static K230LinuxHal *lora_hal;
static Module *lora_module;
static PhysicalLayer *lora_radio;
static SX1262 *lora_sx1262;
static LR2021 *lora_lr2021;
static lv_timer_t *lora_timer;
static lv_timer_t *lora_startup_timer;
static lv_obj_t *lora_chip_label;
static lv_obj_t *lora_status_label;
static lv_obj_t *lora_profile_label;
static lv_obj_t *lora_tx_label;
static lv_obj_t *lora_rx_label;
static lv_obj_t *lora_log_label;
static lv_obj_t *lora_auto_label;
static lv_obj_t *lora_payload_label;
static lv_obj_t *lora_tab_factory;
static lv_obj_t *lora_tab_radio;
static lv_obj_t *lora_tab_log;
static lv_obj_t *lora_factory_panel;
static lv_obj_t *lora_chat_panel;
static lv_obj_t *lora_log_panel;
static lv_obj_t *lora_add_button;
static lv_obj_t *lora_editor_overlay;
static lv_obj_t *lora_carrier_confirm_overlay;
static lv_obj_t *lora_editor_value_labels[9];
static lv_obj_t *lora_editor_delete_button;
static lv_obj_t *lora_profile_buttons[LORA_MAX_PROFILES];
static lv_obj_t *lora_session_buttons[LORA_SESSION_BUTTON_COUNT];
static lv_obj_t *lora_payload_buttons[2];
static lv_obj_t *lora_payload_row_label;
static lv_obj_t *lora_message_page;
static lv_obj_t *lora_message_cont;
static int lora_active_profile = LORA_DEFAULT_PROFILE_INDEX;
static int lora_builtin_profile_count;
static int lora_profile_count;
static int lora_profiles_loaded;
static int lora_editor_source_index = -1;
static int lora_initialized;
static int lora_rx_running;
static int lora_tx_pending;
static int lora_continuous_tx;
static lora_session_mode_t lora_session_mode = LORA_SESSION_LISTEN;
static lora_payload_mode_t lora_payload_mode = LORA_PAYLOAD_COUNTER;
static int lora_auto_tx = LORA_AUTO_TX_DEFAULT;
static int lora_factory_tx_counter;
static int lora_tx_count;
static int lora_rx_count;
static int lora_message_count;
static lora_chip_type_t lora_chip_type = LORA_CHIP_NONE;
static uint64_t lora_last_auto_tx_us;
static char lora_status_text[160] = "Not initialized";
static char lora_payload_text[96] = "hello from k230";
static lora_profile_t lora_editor_profile;
static char lora_log_lines[LORA_MAX_LOG_LINES][LORA_LINE_MAX];
static int lora_log_count;

typedef enum {
    LORA_OP_IDLE = 0,
    LORA_OP_RX,
    LORA_OP_TX,
    LORA_OP_CARRIER,
} lora_op_t;

static volatile unsigned int lora_radio_event_count;
static lora_op_t lora_active_op = LORA_OP_IDLE;

static void lora_update_stats(void);
static void lora_update_session_ui(void);
static void lora_add_message(const char *text, int sent, const char *meta);
static void lora_show_tab(int tab);
static void lora_rebuild_factory_panel(void);
static void lora_refresh_builtin_profiles(lora_chip_type_t chip);

static const char *lora_chip_name(lora_chip_type_t chip)
{
    switch(chip) {
    case LORA_CHIP_SX1262:
        return "SX1262";
    case LORA_CHIP_LR2021:
        return "LR2021";
    case LORA_CHIP_NONE:
    default:
        return "Auto LoRa";
    }
}

static const char *lora_active_chip_name(void)
{
    return lora_chip_name(lora_chip_type);
}

static void lora_update_chip_label(void)
{
    if(lora_chip_label && lv_obj_is_valid(lora_chip_label)) {
        lv_label_set_text(lora_chip_label, lora_active_chip_name());
    }
}

static void lora_radio_event_isr(void)
{
    (void)__sync_fetch_and_add(&lora_radio_event_count, 1U);
}

static unsigned int lora_take_radio_events(void)
{
    return __sync_lock_test_and_set(&lora_radio_event_count, 0U);
}

static void lora_log(const char *fmt, ...)
{
    FILE *fp;
    char line[LORA_LINE_MAX];
    char all[LORA_MAX_LOG_LINES * LORA_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fp = fopen(LORA_LOG_PATH, "a");
    if(fp) {
        fprintf(fp, "%llu %s\n", (unsigned long long)ui_monotonic_us(), line);
        fclose(fp);
    }

    if(lora_log_count < LORA_MAX_LOG_LINES) {
        snprintf(lora_log_lines[lora_log_count++], LORA_LINE_MAX, "%s", line);
    } else {
        memmove(lora_log_lines, lora_log_lines + 1,
                sizeof(lora_log_lines[0]) * (LORA_MAX_LOG_LINES - 1));
        snprintf(lora_log_lines[LORA_MAX_LOG_LINES - 1], LORA_LINE_MAX, "%s",
                 line);
    }

    all[0] = '\0';
    for(int i = 0; i < lora_log_count; i++) {
        strncat(all, lora_log_lines[i], sizeof(all) - strlen(all) - 1U);
        if(i + 1 < lora_log_count) {
            strncat(all, "\n", sizeof(all) - strlen(all) - 1U);
        }
    }

    if(lora_log_label && lv_obj_is_valid(lora_log_label)) {
        lv_label_set_text(lora_log_label, all);
    }
}

static void lora_set_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(lora_status_text, sizeof(lora_status_text), fmt, ap);
    va_end(ap);

    if(lora_status_label && lv_obj_is_valid(lora_status_label)) {
        lv_label_set_text(lora_status_label, ui_tr(lora_status_text));
    }
}

static int lora_clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static float lora_clamp_float(float value, float min_value, float max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static const lora_profile_t *lora_builtin_profiles_for_chip(lora_chip_type_t chip,
                                                            int *count)
{
    if(chip == LORA_CHIP_LR2021) {
        if(count) {
            *count = LORA_LR2021_FACTORY_PROFILE_COUNT;
        }
        return lora_factory_profiles_lr2021;
    }

    if(count) {
        *count = LORA_SUB1G_FACTORY_PROFILE_COUNT;
    }
    return lora_factory_profiles_sub1g;
}

static void lora_refresh_builtin_profiles(lora_chip_type_t chip)
{
    lora_profile_t user_profiles[LORA_MAX_PROFILES];
    int old_builtin = lora_builtin_profile_count;
    int old_active = lora_active_profile;
    int user_count = 0;
    int new_builtin = 0;
    const lora_profile_t *source =
        lora_builtin_profiles_for_chip(chip, &new_builtin);

    if(old_builtin < 0 || old_builtin > lora_profile_count) {
        old_builtin = 0;
    }
    for(int i = old_builtin; i < lora_profile_count && user_count < LORA_MAX_PROFILES; i++) {
        if(!lora_profiles[i].builtin) {
            user_profiles[user_count++] = lora_profiles[i];
            user_profiles[user_count - 1].builtin = 0;
        }
    }

    memset(lora_profiles, 0, sizeof(lora_profiles));
    lora_builtin_profile_count = 0;
    lora_profile_count = 0;

    for(int i = 0; i < new_builtin && lora_profile_count < LORA_MAX_PROFILES; i++) {
        lora_profiles[lora_profile_count] = source[i];
        lora_profiles[lora_profile_count].builtin = 1;
        lora_profile_count++;
    }
    lora_builtin_profile_count = lora_profile_count;

    for(int i = 0; i < user_count && lora_profile_count < LORA_MAX_PROFILES; i++) {
        lora_profiles[lora_profile_count++] = user_profiles[i];
    }

    if(old_active >= old_builtin && old_builtin > 0) {
        int user_index = old_active - old_builtin;
        if(user_index >= 0 && user_index < user_count) {
            lora_active_profile = lora_builtin_profile_count + user_index;
        } else {
            lora_active_profile = LORA_DEFAULT_PROFILE_INDEX;
        }
    } else if(old_active >= 0 && old_active < lora_builtin_profile_count) {
        lora_active_profile = old_active;
    } else {
        lora_active_profile = LORA_DEFAULT_PROFILE_INDEX;
    }

    if(lora_active_profile < 0 || lora_active_profile >= lora_profile_count) {
        lora_active_profile = lora_profile_count > LORA_DEFAULT_PROFILE_INDEX ?
                              LORA_DEFAULT_PROFILE_INDEX : 0;
    }
}

static uint8_t lora_parse_sync_word(const char *text, uint8_t fallback)
{
    char *endp = NULL;
    unsigned long value;

    if(!text || !text[0]) {
        return fallback;
    }
    value = strtoul(text, &endp, 0);
    if(endp == text || value > 255UL) {
        return fallback;
    }
    return (uint8_t)value;
}

static void lora_sanitize_profile(lora_profile_t *profile)
{
    if(!profile) {
        return;
    }
    ui_trim_text(profile->name);
    for(size_t i = 0; profile->name[i]; i++) {
        if(profile->name[i] == '|') {
            profile->name[i] = '_';
        }
    }
    if(!profile->name[0]) {
        snprintf(profile->name, sizeof(profile->name), "User");
    }
    profile->freq = lora_clamp_float(profile->freq, 150.0f, 2500.0f);
    if(profile->freq > 960.0f && profile->freq < 2000.0f) {
        profile->freq = 2400.0f;
    }
    profile->bandwidth = lora_clamp_float(profile->bandwidth, 7.8f, 500.0f);
    profile->power = (int8_t)lora_clamp_int(profile->power,
                                            profile->freq >= 2000.0f ? -19 : -9,
                                            profile->freq >= 2000.0f ?
                                            LORA_LR2021_16E8_HF_POWER_MAX : 22);
    profile->sf = (uint8_t)lora_clamp_int(profile->sf, 5, 12);
    profile->cr = (uint8_t)lora_clamp_int(profile->cr, 5, 8);
    profile->preamble = (uint16_t)lora_clamp_int(profile->preamble, 4, 65535);
    profile->interval_ms =
        (uint32_t)lora_clamp_int((int)profile->interval_ms, 100, 600000);
    profile->counter_payload = profile->counter_payload ? 1 : 0;
}

static void lora_format_profile_summary(const lora_profile_t *profile,
                                        char *out, size_t out_len)
{
    if(!profile || !out || out_len == 0U) {
        return;
    }
    snprintf(out, out_len,
             "%.1f MHz  BW %.1f  PWR %d  SF%u CR4/%u  SW 0x%02X  PRE %u  %ums",
             profile->freq, profile->bandwidth, profile->power, profile->sf,
             profile->cr, profile->sync_word, profile->preamble,
             profile->interval_ms);
}

static void lora_format_editor_value(lora_edit_field_t field, char *out,
                                     size_t out_len)
{
    if(!out || out_len == 0U) {
        return;
    }

    switch(field) {
    case LORA_EDIT_NAME:
        snprintf(out, out_len, "%s", lora_editor_profile.name);
        break;
    case LORA_EDIT_FREQ:
        snprintf(out, out_len, "%.3f", lora_editor_profile.freq);
        break;
    case LORA_EDIT_BW:
        snprintf(out, out_len, "%.2f", lora_editor_profile.bandwidth);
        break;
    case LORA_EDIT_POWER:
        snprintf(out, out_len, "%d", lora_editor_profile.power);
        break;
    case LORA_EDIT_SF:
        snprintf(out, out_len, "%u", lora_editor_profile.sf);
        break;
    case LORA_EDIT_CR:
        snprintf(out, out_len, "%u", lora_editor_profile.cr);
        break;
    case LORA_EDIT_SYNC:
        snprintf(out, out_len, "0x%02X", lora_editor_profile.sync_word);
        break;
    case LORA_EDIT_PREAMBLE:
        snprintf(out, out_len, "%u", lora_editor_profile.preamble);
        break;
    case LORA_EDIT_INTERVAL:
        snprintf(out, out_len, "%u", lora_editor_profile.interval_ms);
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void lora_update_editor_values(void)
{
    char value[96];

    for(int i = 0; i < LORA_EDIT_FIELD_COUNT; i++) {
        if(lora_editor_value_labels[i] &&
           lv_obj_is_valid(lora_editor_value_labels[i])) {
            lora_format_editor_value((lora_edit_field_t)i, value,
                                     sizeof(value));
            lv_label_set_text(lora_editor_value_labels[i], value);
        }
    }
}

static void lora_load_profiles(void)
{
    FILE *fp;
    char line[256];

    if(lora_profiles_loaded) {
        return;
    }
    lora_profiles_loaded = 1;
    lora_refresh_builtin_profiles(lora_chip_type);

    fp = fopen(LORA_PROFILE_STORE_PATH, "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp) &&
          lora_profile_count < LORA_MAX_PROFILES) {
        lora_profile_t profile;
        char name[LORA_PROFILE_NAME_MAX] = {0};
        unsigned sf = 0;
        unsigned cr = 0;
        unsigned sync = 0;
        unsigned preamble = 0;
        unsigned interval = 0;
        int power = 0;
        int counter = 1;

        memset(&profile, 0, sizeof(profile));
        if(sscanf(line, "%23[^|]|%f|%f|%d|%u|%u|%u|%u|%u|%d",
                  name, &profile.freq, &profile.bandwidth, &power, &sf, &cr,
                  &sync, &preamble, &interval, &counter) != 10) {
            continue;
        }
        snprintf(profile.name, sizeof(profile.name), "%s", name);
        profile.power = (int8_t)power;
        profile.sf = (uint8_t)sf;
        profile.cr = (uint8_t)cr;
        profile.sync_word = (uint8_t)sync;
        profile.preamble = (uint16_t)preamble;
        profile.interval_ms = interval;
        profile.counter_payload = counter;
        profile.builtin = 0;
        lora_sanitize_profile(&profile);
        lora_profiles[lora_profile_count++] = profile;
    }

    fclose(fp);
    lora_log("Loaded %d user profiles from %s",
             lora_profile_count - lora_builtin_profile_count,
             LORA_PROFILE_STORE_PATH);
}

static void lora_save_profiles(void)
{
    FILE *fp = fopen(LORA_PROFILE_STORE_PATH, "w");
    if(!fp) {
        lora_log("Profile save failed: %s", strerror(errno));
        return;
    }

    for(int i = lora_builtin_profile_count; i < lora_profile_count; i++) {
        lora_sanitize_profile(&lora_profiles[i]);
        fprintf(fp, "%s|%.3f|%.2f|%d|%u|%u|%u|%u|%u|%d\n",
                lora_profiles[i].name, lora_profiles[i].freq,
                lora_profiles[i].bandwidth, lora_profiles[i].power,
                lora_profiles[i].sf, lora_profiles[i].cr,
                lora_profiles[i].sync_word, lora_profiles[i].preamble,
                lora_profiles[i].interval_ms,
                lora_profiles[i].counter_payload);
    }

    fclose(fp);
    lora_log("Saved %d user profiles to %s",
             lora_profile_count - lora_builtin_profile_count,
             LORA_PROFILE_STORE_PATH);
}

static const char *lora_session_text(void)
{
    switch(lora_session_mode) {
    case LORA_SESSION_AUTO_TX:
        return "Auto TX";
    case LORA_SESSION_CARRIER:
        return "Carrier";
    case LORA_SESSION_LISTEN:
    default:
        return "Listen";
    }
}

static const char *lora_payload_mode_text(void)
{
    return lora_payload_mode == LORA_PAYLOAD_FIXED ? "Fixed" : "Counter";
}

static void lora_update_stats(void)
{
    if(lora_active_profile < 0 || lora_active_profile >= lora_profile_count) {
        lora_active_profile = lora_profile_count > LORA_DEFAULT_PROFILE_INDEX ?
                              LORA_DEFAULT_PROFILE_INDEX : 0;
    }
    const lora_profile_t *profile = &lora_profiles[lora_active_profile];
    char profile_buf[128];
    char tx_buf[48];
    char rx_buf[48];

    snprintf(profile_buf, sizeof(profile_buf),
             "%s  %.1f MHz  BW %.0f  SF%u CR4/%u  SW 0x%02X",
             profile->name, profile->freq, profile->bandwidth,
             profile->sf, profile->cr, profile->sync_word);
    snprintf(tx_buf, sizeof(tx_buf), "TX %d", lora_tx_count);
    snprintf(rx_buf, sizeof(rx_buf), "RX %d", lora_rx_count);

    if(lora_profile_label && lv_obj_is_valid(lora_profile_label)) {
        lv_label_set_text(lora_profile_label, profile_buf);
    }
    if(lora_tx_label && lv_obj_is_valid(lora_tx_label)) {
        lv_label_set_text(lora_tx_label, tx_buf);
    }
    if(lora_rx_label && lv_obj_is_valid(lora_rx_label)) {
        lv_label_set_text(lora_rx_label, rx_buf);
    }
    if(lora_auto_label && lv_obj_is_valid(lora_auto_label)) {
        const char *sub_mode = "RX";
        if(lora_session_mode == LORA_SESSION_AUTO_TX) {
            sub_mode = ui_tr(lora_payload_mode_text());
        } else if(lora_session_mode == LORA_SESSION_CARRIER) {
            sub_mode = "CW";
        }
        lv_label_set_text_fmt(lora_auto_label, "%s  %s",
                              ui_tr(lora_session_text()),
                              sub_mode);
    }
    if(lora_payload_label && lv_obj_is_valid(lora_payload_label)) {
        if(ui_is_landscape()) {
            lv_label_set_text_fmt(lora_payload_label,
                                  "%s %s   %s %s   TX %d  RX %d   %s",
                                  ui_tr("Mode"), ui_tr(lora_session_text()),
                                  ui_tr("Payload"),
                                  ui_tr(lora_payload_mode_text()),
                                  lora_tx_count, lora_rx_count,
                                  lora_payload_text);
        } else {
            lv_label_set_text_fmt(lora_payload_label,
                                  "%s %s  %s %s\n%s TX %d  RX %d\n%s: %s",
                                  ui_tr("Mode"), ui_tr(lora_session_text()),
                                  ui_tr("Payload"),
                                  ui_tr(lora_payload_mode_text()),
                                  ui_tr("Traffic"), lora_tx_count,
                                  lora_rx_count, ui_tr("Payload"),
                                  lora_payload_text);
        }
    }
    lora_update_session_ui();
}

static const char *lora_error_name(int16_t state)
{
    switch(state) {
    case RADIOLIB_ERR_NONE:
        return "ok";
    case RADIOLIB_ERR_CHIP_NOT_FOUND:
        return "chip not found";
    case RADIOLIB_ERR_SPI_WRITE_FAILED:
        return "spi write failed";
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:
        return "spi cmd timeout";
    case RADIOLIB_ERR_SPI_CMD_INVALID:
        return "spi cmd invalid";
    case RADIOLIB_ERR_SPI_CMD_FAILED:
        return "spi cmd failed";
    case RADIOLIB_ERR_TX_TIMEOUT:
        return "tx timeout";
    case RADIOLIB_ERR_RX_TIMEOUT:
        return "rx timeout";
    default:
        return "RadioLib error";
    }
}

static void lora_delete_radio_objects(void)
{
    if(lora_radio) {
        lora_radio->clearPacketReceivedAction();
        lora_radio->clearPacketSentAction();
        (void)lora_radio->standby();
    }
    if(lora_sx1262) {
        delete lora_sx1262;
        lora_sx1262 = NULL;
    }
    if(lora_lr2021) {
        delete lora_lr2021;
        lora_lr2021 = NULL;
    }
    if(lora_module) {
        delete lora_module;
        lora_module = NULL;
    }
    lora_radio = NULL;
    lora_continuous_tx = 0;
    lora_chip_type = LORA_CHIP_NONE;
    lora_update_chip_label();
}

static int lora_hw_prepare(void)
{
    if(lora_initialized && lora_hal && lora_hal->spi_ready()) {
        return 0;
    }

    if(!lora_hal) {
        lora_hal = new K230LinuxHal(LORA_SPI_DEV, LORA_SPI_SPEED_HZ);
    }
    if(!lora_hal) {
        lora_set_status("HAL allocation failed");
        lora_log("HAL allocation failed");
        return -1;
    }

    lora_hal->pinMode(LORA_PIN_POWER, K230_HAL_GPIO_OUTPUT);
    lora_hal->digitalWrite(LORA_PIN_POWER, K230_HAL_GPIO_HIGH);
    lora_hal->delay(30);
    lora_hal->spiBegin();
    if(!lora_hal->spi_ready()) {
        lora_set_status("%s", lora_hal->last_error());
        lora_log("SPI not ready: %s", lora_hal->last_error());
        return -1;
    }

    lora_initialized = 1;
    lora_log("Hardware prepared: spi=%s hw_cs=io%u rst=io%u busy=io%u irq_gpio=io%u lr2021_irq_dio=%u pwr=io%u",
             LORA_SPI_DEV, LORA_PIN_CS, LORA_PIN_RST, LORA_PIN_BUSY,
             LORA_PIN_DIO1, LORA_LR2021_IRQ_DIO_NUM, LORA_PIN_POWER);
    return 0;
}

static int lora_profile_requires_lr2021(const lora_profile_t *profile)
{
    return profile && profile->freq >= 2000.0f;
}

static int lora_lr2021_should_retry_xtal(int16_t state)
{
    return state == RADIOLIB_ERR_SPI_CMD_INVALID ||
           state == RADIOLIB_ERR_SPI_CMD_FAILED;
}

static int16_t lora_lr2021_begin_config(const lora_profile_t *profile,
                                        float tcxo_voltage)
{
    ConfigLoRa_t config;

    config.frequency = profile->freq;
    config.bandwidth = profile->bandwidth;
    config.spreadingFactor = profile->sf;
    config.codingRate = profile->cr;
    config.syncWord = profile->sync_word;
    config.power = profile->power;
    config.preambleLength = profile->preamble;
    lora_lr2021->tcxoVoltage = tcxo_voltage;
    return lora_lr2021->begin(config);
}

static void lora_lr2021_apply_16e8_rf_switch(void)
{
    if(!lora_lr2021) {
        return;
    }
    lora_lr2021->setRfSwitchTable(lora_lr2021_16e8_rf_switch_dio_pins,
                                  lora_lr2021_16e8_rf_switch_table);
    lora_log("LR2021 16E8 RF switch: sub1G TX/RX DIO6=0 DIO7=0, 2.4G TX DIO6=0 DIO7=1, RX DIO6=1 DIO7=0");
}

static int16_t lora_lr2021_set_frequency_logged(const lora_profile_t *profile,
                                                int high_freq)
{
    int16_t state = lora_lr2021->setFrequency(profile->freq);
    lora_log("LR2021 setFrequency %.1f state=%d %s",
             profile->freq, state, lora_error_name(state));
    if(high_freq && lora_lr2021_should_retry_xtal(state)) {
        lora_log("LR2021 HF setFrequency retry skipCalibration after state=%d %s",
                 state, lora_error_name(state));
        state = lora_lr2021->setFrequency(profile->freq, true);
        lora_log("LR2021 setFrequency %.1f skipCalibration state=%d %s",
                 profile->freq, state, lora_error_name(state));
    }
    return state;
}

static int16_t lora_lr2021_set_16e8_hf_power(int8_t power)
{
    int8_t safe_power = (int8_t)lora_clamp_int(power, -19,
                                               LORA_LR2021_16E8_HF_POWER_MAX);
    int16_t state = lora_lr2021->setOutputPower(safe_power);
    lora_log("LR2021 16E8 HF setOutputPower safe=%d state=%d %s",
             safe_power, state, lora_error_name(state));
    if(state == RADIOLIB_ERR_SPI_CMD_INVALID) {
        lora_log("LR2021 16E8 HF setOutputPower state=%d ignored to match LilyGo Factory; continue with TX/RX", state);
        return RADIOLIB_ERR_NONE;
    }
    return state;
}

static int16_t lora_lr2021_apply_profile_params(const lora_profile_t *profile,
                                                int high_freq)
{
    int16_t state = lora_lr2021_set_frequency_logged(profile, high_freq);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setBandwidth(profile->bandwidth);
    lora_log("LR2021 setBandwidth %.1f state=%d %s",
             profile->bandwidth, state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setSpreadingFactor(profile->sf);
    lora_log("LR2021 setSpreadingFactor SF%u state=%d %s",
             profile->sf, state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setCodingRate(profile->cr);
    lora_log("LR2021 setCodingRate CR4/%u state=%d %s",
             profile->cr, state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setSyncWord(profile->sync_word);
    lora_log("LR2021 setSyncWord 0x%02X state=%d %s",
             profile->sync_word, state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    if(high_freq) {
        state = lora_lr2021_set_16e8_hf_power(profile->power);
    } else {
        state = lora_lr2021->setOutputPower(profile->power);
        lora_log("LR2021 setOutputPower pwr=%d band=sub1G state=%d %s",
                 profile->power, state, lora_error_name(state));
    }
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setPreambleLength(profile->preamble);
    lora_log("LR2021 setPreambleLength %u state=%d %s",
             profile->preamble, state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setCRC(0);
    lora_log("LR2021 setCRC off state=%d %s", state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    if(high_freq) {
        int16_t rx_state = lora_lr2021->setRxBoostedGainMode(4);
        lora_log("LR2021 HF setRxBoostedGainMode level=4 state=%d %s ignored for Factory-compatible flow",
                 rx_state, lora_error_name(rx_state));
    }
    return RADIOLIB_ERR_NONE;
}

static int16_t lora_lr2021_bootstrap_for_16e8(float tcxo_voltage)
{
    lora_profile_t bootstrap;

    memset(&bootstrap, 0, sizeof(bootstrap));
    snprintf(bootstrap.name, sizeof(bootstrap.name), "LR2021 bootstrap");
    bootstrap.freq = LORA_LR2021_BOOTSTRAP_FREQ;
    bootstrap.bandwidth = LORA_LR2021_BOOTSTRAP_BW;
    bootstrap.power = LORA_LR2021_16E8_HF_POWER_DEFAULT;
    bootstrap.sf = LORA_LR2021_BOOTSTRAP_SF;
    bootstrap.cr = LORA_LR2021_BOOTSTRAP_CR;
    bootstrap.sync_word = LORA_LR2021_BOOTSTRAP_SW;
    bootstrap.preamble = LORA_LR2021_BOOTSTRAP_PRE;
    lora_log("LR2021 16E8 bootstrap sub1G freq=%.1f bw=%.1f sf=%u cr=4/%u sw=0x%02X pwr=%d pre=%u tcxo=%.1f",
             bootstrap.freq, bootstrap.bandwidth, bootstrap.sf, bootstrap.cr,
             bootstrap.sync_word, bootstrap.power, bootstrap.preamble,
             tcxo_voltage);
    return lora_lr2021_begin_config(&bootstrap, tcxo_voltage);
}

static int16_t lora_begin_active_chip(const lora_profile_t *profile)
{
    int16_t state = RADIOLIB_ERR_CHIP_NOT_FOUND;
    int high_freq = 0;

    if(!profile || !lora_radio) {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }

    switch(lora_chip_type) {
    case LORA_CHIP_SX1262:
        if(!lora_sx1262) {
            return RADIOLIB_ERR_CHIP_NOT_FOUND;
        }
        state = lora_sx1262->begin(profile->freq, profile->bandwidth,
                                   profile->sf, profile->cr,
                                   profile->sync_word, profile->power,
                                   profile->preamble, 3.3f, false);
        if(state == RADIOLIB_ERR_NONE) {
            state = lora_sx1262->setCRC(0);
        }
        break;
    case LORA_CHIP_LR2021:
        if(!lora_lr2021) {
            return RADIOLIB_ERR_CHIP_NOT_FOUND;
        }
        high_freq = profile->freq >= 2000.0f;
        lora_lr2021->irqDioNum = LORA_LR2021_IRQ_DIO_NUM;
        lora_log("LR2021 begin profile=%s freq=%.1f bw=%.1f sf=%u cr=4/%u sw=0x%02X pwr=%d pre=%u tcxo=auto band=%s",
                 profile->name, profile->freq, profile->bandwidth,
                 profile->sf, profile->cr, profile->sync_word,
                 profile->power, profile->preamble,
                 high_freq ? "2.4G" : "sub1G");
        if(high_freq) {
            state = lora_lr2021_bootstrap_for_16e8(3.0f);
            lora_log("LR2021 HF bootstrap tcxo=3.0 state=%d %s",
                     state, lora_error_name(state));
            if(lora_lr2021_should_retry_xtal(state)) {
                lora_log("LR2021 HF bootstrap retry as XTAL tcxo=0 after state=%d %s",
                         state, lora_error_name(state));
                state = lora_lr2021_bootstrap_for_16e8(0.0f);
                lora_log("LR2021 HF bootstrap tcxo=0 state=%d %s",
                         state, lora_error_name(state));
            }
            if(state == RADIOLIB_ERR_NONE) {
                lora_lr2021_apply_16e8_rf_switch();
                state = lora_lr2021_apply_profile_params(profile, high_freq);
            }
        } else {
            state = lora_lr2021->begin(profile->freq, profile->bandwidth,
                                       profile->sf, profile->cr,
                                       profile->sync_word, profile->power,
                                       profile->preamble, 3.0f);
            if(state == RADIOLIB_ERR_NONE) {
                lora_lr2021_apply_16e8_rf_switch();
            }
        }
        if(state == RADIOLIB_ERR_NONE) {
            if(high_freq) {
                lora_log("LR2021 HF configured modulation bw=%.1f sf=%u cr=4/%u sw=0x%02X pre=%u pwr=%d",
                         profile->bandwidth, profile->sf, profile->cr,
                         profile->sync_word, profile->preamble,
                         profile->power);
            } else {
                state = lora_lr2021->setOutputPower(profile->power);
                lora_log("LR2021 setOutputPower pwr=%d state=%d %s",
                         profile->power, state, lora_error_name(state));
            }
        }
        if(state == RADIOLIB_ERR_NONE && !high_freq) {
            state = lora_lr2021->setCRC(0);
            lora_log("LR2021 setCRC off state=%d %s",
                     state, lora_error_name(state));
        }
        break;
    case LORA_CHIP_NONE:
    default:
        break;
    }
    return state;
}

static int lora_create_radio_candidate(lora_chip_type_t chip)
{
    lora_delete_radio_objects();

    if(!lora_hal) {
        return -1;
    }

    lora_module = new Module(lora_hal, RADIOLIB_NC, LORA_PIN_DIO1,
                             LORA_PIN_RST, LORA_PIN_BUSY);
    if(!lora_module) {
        return -1;
    }

    if(chip == LORA_CHIP_SX1262) {
        lora_sx1262 = new SX1262(lora_module);
        lora_radio = lora_sx1262;
    } else if(chip == LORA_CHIP_LR2021) {
        lora_lr2021 = new LR2021(lora_module);
        lora_lr2021->irqDioNum = LORA_LR2021_IRQ_DIO_NUM;
        lora_log("LR2021 irqDioNum=%u host_irq_gpio=%u set before begin",
                 LORA_LR2021_IRQ_DIO_NUM, LORA_PIN_DIO1);
        lora_radio = lora_lr2021;
    }
    if(!lora_radio) {
        lora_delete_radio_objects();
        return -1;
    }

    lora_chip_type = chip;
    lora_update_chip_label();
    return 0;
}

static int lora_probe_chip(lora_chip_type_t chip, const lora_profile_t *profile,
                           int16_t *state_out)
{
    int16_t state;

    if(lora_create_radio_candidate(chip) != 0) {
        if(state_out) {
            *state_out = RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
        }
        return -1;
    }

    lora_hal->delay(20);
    state = lora_begin_active_chip(profile);
    if(state_out) {
        *state_out = state;
    }
    if(state != RADIOLIB_ERR_NONE) {
        lora_log("Probe %s failed: %d %s", lora_chip_name(chip), state,
                 lora_error_name(state));
        lora_delete_radio_objects();
        lora_hal->delay(20);
        return -1;
    }

    lora_log("Detected %s", lora_chip_name(chip));
    return 0;
}

static int lora_detect_radio(const lora_profile_t *profile)
{
    int16_t sx_state = RADIOLIB_ERR_CHIP_NOT_FOUND;
    int16_t lr_state = RADIOLIB_ERR_CHIP_NOT_FOUND;
    int force_lr2021 = lora_profile_requires_lr2021(profile);

    if(!force_lr2021 &&
       lora_probe_chip(LORA_CHIP_SX1262, profile, &sx_state) == 0) {
        lora_refresh_builtin_profiles(LORA_CHIP_SX1262);
        lora_rebuild_factory_panel();
        return 0;
    }
    if(force_lr2021) {
        lora_log("Profile %.1f MHz requires LR2021, skip SX1262 probe",
                 profile ? profile->freq : 0.0f);
    }
    if(lora_probe_chip(LORA_CHIP_LR2021, profile, &lr_state) == 0) {
        lora_refresh_builtin_profiles(LORA_CHIP_LR2021);
        lora_rebuild_factory_panel();
        return 0;
    }

    if(force_lr2021) {
        lora_set_status("LR2021 probe failed:%d", lr_state);
        lora_log("Probe failed: LR2021=%d %s (SX1262 skipped for %.1f MHz)",
                 lr_state, lora_error_name(lr_state),
                 profile ? profile->freq : 0.0f);
    } else {
        lora_set_status("Probe failed SX1262:%d LR2021:%d", sx_state, lr_state);
        lora_log("Probe failed: SX1262=%d %s LR2021=%d %s",
                 sx_state, lora_error_name(sx_state),
                 lr_state, lora_error_name(lr_state));
    }
    return -1;
}

static int lora_start_rx(void)
{
    int16_t state;

    if(!lora_radio) {
        return -1;
    }

    lora_active_op = LORA_OP_IDLE;
    lora_radio->setPacketReceivedAction(lora_radio_event_isr);
    state = lora_radio->startReceive();
    if(state != RADIOLIB_ERR_NONE) {
        lora_rx_running = 0;
        lora_set_status("RX start failed: %d %s", state,
                        lora_error_name(state));
        lora_log("RX start failed: %d %s", state, lora_error_name(state));
        return -1;
    }

    lora_rx_running = 1;
    lora_active_op = LORA_OP_RX;
    if(lora_session_mode == LORA_SESSION_AUTO_TX) {
        lora_set_status("Auto TX %ums %s", lora_profiles[lora_active_profile].interval_ms,
                        lora_payload_mode_text());
    } else if(lora_session_mode == LORA_SESSION_CARRIER) {
        lora_set_status("Carrier ready");
    } else {
        lora_set_status("Listening");
    }
    return 0;
}

static void lora_abort_radio_op(void)
{
    lora_rx_running = 0;
    lora_tx_pending = 0;
    lora_continuous_tx = 0;
    lora_active_op = LORA_OP_IDLE;
    lora_take_radio_events();
    if(lora_radio) {
        lora_radio->clearPacketReceivedAction();
        lora_radio->clearPacketSentAction();
        (void)lora_radio->standby();
    }
}

static int lora_apply_profile(int index)
{
    const lora_profile_t *profile;
    int16_t state;

    if(index < 0 || index >= lora_profile_count) {
        return -1;
    }
    lora_active_profile = index;
    lora_update_stats();

    if(lora_hw_prepare() != 0) {
        return -1;
    }

    profile = &lora_profiles[lora_active_profile];
    lora_abort_radio_op();
    if(!lora_radio && lora_detect_radio(profile) != 0) {
        return -1;
    }
    profile = &lora_profiles[lora_active_profile];
    state = lora_begin_active_chip(profile);
    if(state != RADIOLIB_ERR_NONE) {
        lora_log("%s re-init failed profile=%s state=%d %s, retry probe",
                 lora_active_chip_name(), profile->name, state,
                 lora_error_name(state));
        if(lora_profile_requires_lr2021(profile) ||
           lora_chip_type == LORA_CHIP_LR2021) {
            int16_t lr_state = RADIOLIB_ERR_CHIP_NOT_FOUND;
            lora_log("Retry LR2021 only for profile=%s freq=%.1f",
                     profile->name, profile->freq);
            if(lora_probe_chip(LORA_CHIP_LR2021, profile, &lr_state) == 0) {
                lora_refresh_builtin_profiles(LORA_CHIP_LR2021);
                lora_rebuild_factory_panel();
                state = RADIOLIB_ERR_NONE;
            } else {
                state = lr_state;
            }
        } else {
            if(lora_detect_radio(profile) == 0) {
                state = RADIOLIB_ERR_NONE;
            }
        }
    }
    if(state != RADIOLIB_ERR_NONE) {
        lora_set_status("Init failed: %d %s", state, lora_error_name(state));
        lora_log("Init failed profile=%s state=%d %s", profile->name, state,
                 lora_error_name(state));
        return -1;
    }

    lora_log("Profile applied: chip=%s %s freq=%.1f bw=%.0f sf=%u cr=4/%u sw=0x%02X",
             lora_active_chip_name(), profile->name, profile->freq,
             profile->bandwidth, profile->sf, profile->cr,
             profile->sync_word);
    lora_start_rx();
    return 0;
}

static int lora_start_continuous_carrier(void)
{
    int16_t state;

    if((!lora_initialized || !lora_radio) &&
       lora_apply_profile(lora_active_profile) != 0) {
        return -1;
    }
    if(!lora_radio) {
        lora_set_status("Carrier failed: no radio");
        lora_log("Carrier failed: no radio");
        return -1;
    }

    lora_abort_radio_op();
    state = lora_radio->standby();
    if(state == RADIOLIB_ERR_NONE) {
        state = lora_radio->transmitDirect();
    }
    if(state != RADIOLIB_ERR_NONE) {
        lora_set_status("Carrier failed: %d %s", state, lora_error_name(state));
        lora_log("Carrier failed profile=%s state=%d %s",
                 lora_profiles[lora_active_profile].name, state,
                 lora_error_name(state));
        lora_start_rx();
        return -1;
    }

    lora_rx_running = 0;
    lora_tx_pending = 0;
    lora_continuous_tx = 1;
    lora_active_op = LORA_OP_CARRIER;
    lora_set_status("Continuous carrier");
    lora_log("Carrier started chip=%s profile=%s freq=%.1f power=%d",
             lora_active_chip_name(), lora_profiles[lora_active_profile].name,
             lora_profiles[lora_active_profile].freq,
             lora_profiles[lora_active_profile].power);
    return 0;
}

static int lora_send_payload(const char *payload, int from_auto_tx)
{
    int16_t state;
    size_t len;
    char tx_payload[96];

    if(!payload || !payload[0]) {
        payload = "hello from k230";
    }
    snprintf(tx_payload, sizeof(tx_payload), "%s", payload);
    if((!lora_initialized || !lora_radio) &&
       lora_apply_profile(lora_active_profile) != 0) {
        return -1;
    }
    if(lora_continuous_tx) {
        lora_abort_radio_op();
    }
    if(!lora_radio || lora_tx_pending) {
        lora_set_status("TX busy");
        lora_log("TX busy: %s", tx_payload);
        return -1;
    }

    len = strlen(tx_payload);
    if(len > 240U) {
        len = 240U;
    }

    lora_rx_running = 0;
    lora_active_op = LORA_OP_IDLE;
    lora_take_radio_events();
    state = lora_radio->standby();
    if(state == RADIOLIB_ERR_NONE) {
        lora_radio->setPacketSentAction(lora_radio_event_isr);
        state = lora_radio->startTransmit((const uint8_t *)tx_payload, len);
    }
    if(state != RADIOLIB_ERR_NONE) {
        lora_set_status("TX start failed: %d %s", state, lora_error_name(state));
        lora_log("TX start failed: %d %s payload=%s", state,
                 lora_error_name(state), tx_payload);
        lora_start_rx();
        return -1;
    }

    lora_tx_pending = 1;
    lora_active_op = LORA_OP_TX;
    lora_set_status("TX pending");
    lora_add_message(tx_payload, 1, from_auto_tx ? "auto" : "local");
    lora_log("TX start%s: %s", from_auto_tx ? " auto" : "", tx_payload);
    return 0;
}

static void lora_handle_rx_event(void)
{
    uint8_t data[96];
    size_t len;
    int16_t state;

    if(!lora_radio || !lora_rx_running) {
        return;
    }

    memset(data, 0, sizeof(data));
    len = lora_radio->getPacketLength();
    if(len >= sizeof(data)) {
        len = sizeof(data) - 1U;
    }
    state = lora_radio->readData(data, len);
    lora_rx_running = 0;
    lora_active_op = LORA_OP_IDLE;
    if(state == RADIOLIB_ERR_NONE) {
        float rssi = lora_radio->getRSSI();
        float snr = lora_radio->getSNR();
        char meta[48];
        for(size_t i = 0; i < len; i++) {
            if(data[i] < 32U || data[i] > 126U) {
                data[i] = '.';
            }
        }
        data[len] = '\0';
        (void)lora_radio->finishReceive();
        lora_rx_count++;
        lora_set_status("RX %.1f dBm %.1f dB", rssi, snr);
        snprintf(meta, sizeof(meta), "%.1f dBm  %.1f dB", rssi, snr);
        lora_add_message((const char *)data, 0, meta);
        lora_log("RX len=%u rssi=%.1f snr=%.1f data=%s",
                 (unsigned)len, rssi, snr, data);
    } else {
        (void)lora_radio->finishReceive();
        lora_set_status("RX read failed: %d %s", state,
                        lora_error_name(state));
        lora_log("RX read failed: %d %s", state, lora_error_name(state));
    }
    lora_update_stats();
    lora_start_rx();
}

static void lora_handle_tx_event(void)
{
    int16_t state;

    if(!lora_radio || !lora_tx_pending) {
        return;
    }

    state = lora_radio->finishTransmit();
    lora_tx_pending = 0;
    lora_active_op = LORA_OP_IDLE;
    if(state == RADIOLIB_ERR_NONE) {
        lora_tx_count++;
        lora_set_status("TX done");
        lora_log("TX done");
    } else {
        lora_set_status("TX finish failed: %d %s", state,
                        lora_error_name(state));
        lora_log("TX finish failed: %d %s", state, lora_error_name(state));
    }
    lora_update_stats();
    lora_start_rx();
}

static void lora_handle_radio_event(void)
{
    if(!lora_radio) {
        return;
    }

    if(lora_active_op == LORA_OP_TX) {
        lora_handle_tx_event();
    } else if(lora_active_op == LORA_OP_RX) {
        lora_handle_rx_event();
    } else if(lora_active_op == LORA_OP_CARRIER) {
        return;
    } else {
        lora_log("Ignored stale DIO1 event");
    }
}

static void lora_build_auto_payload(char *out, size_t out_len)
{
    if(!out || out_len == 0U) {
        return;
    }
    if(lora_payload_mode == LORA_PAYLOAD_FIXED) {
        snprintf(out, out_len, "%s", LORA_FIXED_PAYLOAD_DEFAULT);
        return;
    }
    snprintf(out, out_len, "%d", ++lora_factory_tx_counter);
}

static void lora_timer_cb(lv_timer_t *timer)
{
    const lora_profile_t *profile = &lora_profiles[lora_active_profile];
    uint64_t now = ui_monotonic_us();
    char payload[96];
    unsigned int events;

    (void)timer;
    events = lora_take_radio_events();
    if(events > 0U) {
        lora_handle_radio_event();
        if(events > 1U) {
            lora_log("Collapsed %u DIO1 events", events);
        }
    }

    if(lora_auto_tx && lora_session_mode == LORA_SESSION_AUTO_TX &&
       !lora_tx_pending &&
       now - lora_last_auto_tx_us >= (uint64_t)profile->interval_ms * 1000ULL) {
        lora_last_auto_tx_us = now;
        lora_build_auto_payload(payload, sizeof(payload));
        lora_send_payload(payload, 1);
    }
}

static void lora_startup_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    lora_startup_timer = NULL;
    lora_set_status("Starting radio...");
    lora_log("Deferred radio start profile=%s",
             lora_profiles[lora_active_profile].name);
    lora_apply_profile(lora_active_profile);
}

static lv_obj_t *lora_button(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *text, uint32_t color)
{
    lv_obj_t *btn = lv_obj_create(parent);
    int label_w = w - 16;

    if(label_w < 40) {
        label_w = 40;
    }

    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222A34), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x303C49), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, h > 62 ? 8 : 6, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2F3A46), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 6);

    lv_obj_t *label = ui_label(btn, text, &lv_font_montserrat_18, color);
    lv_obj_set_width(label, label_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    ui_make_click_forwarder(label);
    return btn;
}

static int lora_panel_content_width(lv_obj_t *panel)
{
    int w = panel ? lv_obj_get_width(panel) - 32 : ui_inner_width();

    if(panel) {
        lv_obj_update_layout(panel);
        w = lv_obj_get_width(panel) - 32;
    }
    if(w < 240) {
        w = 240;
    }
    return w;
}

static int lora_landscape_left_w(void)
{
    int left_w = ui_screen_width() / 4;

    if(left_w < 292) {
        left_w = 292;
    }
    if(left_w > 316) {
        left_w = 316;
    }
    return left_w;
}

static int lora_landscape_right_x(void)
{
    return 24 + lora_landscape_left_w() + 24;
}

static int lora_landscape_right_w(void)
{
    int right_w = ui_screen_width() - lora_landscape_right_x() - 24;

    return right_w > 520 ? right_w : 520;
}

static int lora_landscape_panel_h(void)
{
    int h = ui_body_height(144) - 48;

    return h > 360 ? h : 360;
}

static void lora_set_hidden(lv_obj_t *obj, int hidden)
{
    if(!obj || !lv_obj_is_valid(obj)) {
        return;
    }
    if(hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lora_style_button_selected(lv_obj_t *btn, int selected,
                                       uint32_t color)
{
    if(!btn || !lv_obj_is_valid(btn)) {
        return;
    }
    lv_obj_set_style_bg_color(btn,
                              lv_color_hex(selected ? color : 0x222A34), 0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(selected ? color : 0x2F3A46),
                                  0);
    uint32_t text_color = selected ? 0xFFFFFF : 0xF2F5F8;
    uint32_t child_count = lv_obj_get_child_count(btn);
    for(uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        if(child) {
            lv_obj_set_style_text_color(child, lv_color_hex(text_color), 0);
        }
    }
}

static void lora_reflow_chat_layout(void)
{
    int landscape = ui_is_landscape();
    int panel_h;
    int inner_w;
    int msg_y;
    int msg_h;

    if(!lora_chat_panel || !lora_message_page ||
       !lv_obj_is_valid(lora_chat_panel) ||
       !lv_obj_is_valid(lora_message_page)) {
        return;
    }

    lv_obj_update_layout(lora_chat_panel);
    panel_h = lv_obj_get_height(lora_chat_panel);
    inner_w = lora_panel_content_width(lora_chat_panel);
    msg_y = landscape ? 164 : 234;

    lv_obj_set_pos(lora_message_page, 0, msg_y);
    lv_obj_set_width(lora_message_page, inner_w);

    msg_h = panel_h - msg_y - 4;
    if(msg_h < 118) {
        msg_h = 118;
    }
    lv_obj_set_height(lora_message_page, msg_h);

    lv_obj_move_foreground(lora_message_page);
}

static void lora_update_session_ui(void)
{
    uint32_t mode_colors[LORA_SESSION_BUTTON_COUNT] = {
        0x25C281, 0xF5A524, 0xEF4D5A
    };

    for(size_t i = 0; i < sizeof(lora_profile_buttons) / sizeof(lora_profile_buttons[0]); i++) {
        lora_style_button_selected(lora_profile_buttons[i],
                                   (int)i == lora_active_profile, 0x7C3AED);
    }
    for(int i = 0; i < LORA_SESSION_BUTTON_COUNT; i++) {
        lora_style_button_selected(lora_session_buttons[i],
                                   i == (int)lora_session_mode,
                                   mode_colors[i]);
    }
    for(int i = 0; i < 2; i++) {
        lora_style_button_selected(lora_payload_buttons[i],
                                   i == (int)lora_payload_mode, 0x7C3AED);
    }

    lora_set_hidden(lora_payload_row_label,
                    lora_session_mode != LORA_SESSION_AUTO_TX);
    lora_set_hidden(lora_payload_buttons[0],
                    lora_session_mode != LORA_SESSION_AUTO_TX);
    lora_set_hidden(lora_payload_buttons[1],
                    lora_session_mode != LORA_SESSION_AUTO_TX);

    lora_reflow_chat_layout();
}

static void lora_add_message(const char *text, int sent, const char *meta)
{
    int page_w;
    int bubble_w;
    int text_w;

    if(!lora_message_page || !lora_message_cont || !text || !text[0]) {
        return;
    }

    lv_obj_update_layout(lora_message_page);
    page_w = lv_obj_get_width(lora_message_page);
    bubble_w = page_w - 28;
    if(bubble_w < 260) {
        bubble_w = 260;
    }
    if(bubble_w > 720) {
        bubble_w = 720;
    }
    text_w = bubble_w - 16;

    if(lora_message_count >= LORA_MAX_MESSAGES) {
        lv_obj_t *first = lv_obj_get_child(lora_message_cont, 0);
        if(first) {
            lv_obj_delete(first);
            lora_message_count--;
        }
    }

    lv_obj_t *row = lv_obj_create(lora_message_cont);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_top(row, 3, 0);
    lv_obj_set_style_pad_bottom(row, 3, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, sent ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, bubble_w);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(sent ? 0x2563EB : 0x222A34), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_style_pad_row(bubble, 4, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *body = ui_label(bubble, text, &lv_font_montserrat_16, 0xFFFFFF);
    lv_obj_set_width(body, text_w);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    lv_obj_t *footer = ui_label(bubble, meta && meta[0] ? meta :
                                (sent ? "sent" : "received"),
                                &lv_font_montserrat_14,
                                sent ? 0xDDEBFF : 0x9AA4AF);
    lv_obj_set_width(footer, text_w);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_DOT);

    lora_message_count++;
    lv_obj_scroll_to_y(lora_message_page, lv_obj_get_height(lora_message_cont),
                       LV_ANIM_ON);
}

static int lora_enter_listen_mode(void)
{
    lora_session_mode = LORA_SESSION_LISTEN;
    lora_auto_tx = 0;
    lora_last_auto_tx_us = 0;
    lora_abort_radio_op();
    int rc = lora_apply_profile(lora_active_profile);
    if(rc == 0) {
        lora_add_message("Listening with selected profile.", 0, "local");
        lora_log("Mode Listen profile=%s",
                 lora_profiles[lora_active_profile].name);
    }
    lora_update_stats();
    return rc;
}

static int lora_enter_auto_tx_mode(void)
{
    lora_session_mode = LORA_SESSION_AUTO_TX;
    lora_auto_tx = 1;
    lora_last_auto_tx_us = 0;
    lora_factory_tx_counter = 0;
    lora_abort_radio_op();
    int rc = lora_apply_profile(lora_active_profile);
    if(rc == 0) {
        lora_add_message("Auto TX started.", 0, lora_payload_mode_text());
        lora_log("Mode Auto TX profile=%s payload=%s interval=%ums",
                 lora_profiles[lora_active_profile].name,
                 lora_payload_mode_text(),
                 lora_profiles[lora_active_profile].interval_ms);
    } else {
        lora_session_mode = LORA_SESSION_LISTEN;
        lora_auto_tx = 0;
    }
    lora_update_stats();
    return rc;
}

static int lora_enter_carrier_mode(void)
{
    lora_session_mode = LORA_SESSION_CARRIER;
    lora_auto_tx = 0;
    lora_last_auto_tx_us = 0;
    lora_factory_tx_counter = 0;
    int rc = lora_start_continuous_carrier();
    if(rc == 0) {
        lora_add_message("Continuous carrier started.", 1, "CW");
    } else {
        lora_session_mode = LORA_SESSION_LISTEN;
        lora_auto_tx = 0;
    }
    lora_update_stats();
    return rc;
}

static void lora_close_carrier_confirm(void)
{
    if(lora_carrier_confirm_overlay &&
       lv_obj_is_valid(lora_carrier_confirm_overlay)) {
        lv_obj_delete(lora_carrier_confirm_overlay);
    }
    lora_carrier_confirm_overlay = NULL;
}

static void lora_carrier_cancel_event_cb(lv_event_t *event)
{
    (void)event;
    lora_close_carrier_confirm();
    lora_log("Carrier warning cancelled");
}

static void lora_carrier_confirm_event_cb(lv_event_t *event)
{
    (void)event;
    lora_close_carrier_confirm();
    lora_log("Carrier warning confirmed");
    lora_enter_carrier_mode();
}

static void lora_open_carrier_confirm(void)
{
    lv_obj_t *dialog;
    lv_obj_t *label;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    int dialog_w = landscape ? 520 : screen_w - 48;
    int dialog_h = 284;
    int pad = 24;
    int gap = 18;
    int button_w;

    if(lora_session_mode == LORA_SESSION_CARRIER && lora_continuous_tx) {
        return;
    }

    if(dialog_w > screen_w - 32) {
        dialog_w = screen_w - 32;
    }
    if(dialog_w < 300) {
        dialog_w = 300;
    }
    if(dialog_h > screen_h - 32) {
        dialog_h = screen_h - 32;
    }
    button_w = (dialog_w - pad * 2 - gap) / 2;

    lora_close_carrier_confirm();

    lora_carrier_confirm_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(lora_carrier_confirm_overlay);
    lv_obj_set_style_bg_color(lora_carrier_confirm_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lora_carrier_confirm_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(lora_carrier_confirm_overlay, 0, 0);
    lv_obj_set_style_pad_all(lora_carrier_confirm_overlay, 0, 0);
    lv_obj_clear_flag(lora_carrier_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(lora_carrier_confirm_overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_set_size(dialog, dialog_w, dialog_h);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0xEF4D5A), 0);
    lv_obj_set_style_border_width(dialog, 1, 0);

    label = ui_label(dialog, ui_tr("Antenna required"),
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_width(label, dialog_w - pad * 2);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, pad, 24);

    label = ui_label(dialog,
                     ui_tr("Connect a suitable antenna before enabling continuous carrier transmission."),
                     &lv_font_montserrat_18, 0xC9D3DF);
    lv_obj_set_width(label, dialog_w - pad * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, pad, 76);

    label = ui_label(dialog, ui_tr("Running this test without an antenna can damage the radio front end."),
                     &lv_font_montserrat_16, 0xF5A524);
    lv_obj_set_width(label, dialog_w - pad * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, pad, 142);

    btn = ui_command_button(dialog, pad, dialog_h - 76, button_w,
                            ui_tr("Cancel"), 0x64748B);
    lv_obj_add_event_cb(btn, lora_carrier_cancel_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(dialog, pad + button_w + gap, dialog_h - 76,
                            button_w, ui_tr("OK"), 0xEF4D5A);
    lv_obj_add_event_cb(btn, lora_carrier_confirm_event_cb, LV_EVENT_CLICKED,
                        NULL);
}

static const char *lora_editor_field_title(lora_edit_field_t field)
{
    switch(field) {
    case LORA_EDIT_NAME:
        return "Name";
    case LORA_EDIT_FREQ:
        return "Frequency MHz";
    case LORA_EDIT_BW:
        return "Bandwidth kHz";
    case LORA_EDIT_POWER:
        return "TX Power dBm";
    case LORA_EDIT_SF:
        return "Spreading Factor";
    case LORA_EDIT_CR:
        return "Coding Rate";
    case LORA_EDIT_SYNC:
        return "Sync Word";
    case LORA_EDIT_PREAMBLE:
        return "Preamble";
    case LORA_EDIT_INTERVAL:
        return "Auto TX Interval ms";
    default:
        return "Field";
    }
}

static const char *lora_editor_field_hint(lora_edit_field_t field)
{
    switch(field) {
    case LORA_EDIT_NAME:
        return "Profile name";
    case LORA_EDIT_FREQ:
        return "150.000 - 960.000 or 2400.000 - 2500.000";
    case LORA_EDIT_BW:
        return "7.80 - 500.00";
    case LORA_EDIT_POWER:
        return "-9 - 22, 2.4G safe max 9";
    case LORA_EDIT_SF:
        return "5 - 12";
    case LORA_EDIT_CR:
        return "5 - 8";
    case LORA_EDIT_SYNC:
        return "0x00 - 0xFF";
    case LORA_EDIT_PREAMBLE:
        return "4 - 65535";
    case LORA_EDIT_INTERVAL:
        return "100 - 600000";
    default:
        return "";
    }
}

static void lora_editor_close(void)
{
    ui_input_dialog_close_active();
    if(lora_editor_overlay && lv_obj_is_valid(lora_editor_overlay)) {
        lv_obj_delete(lora_editor_overlay);
    }
    lora_editor_overlay = NULL;
    lora_editor_delete_button = NULL;
    lora_editor_source_index = -1;
    memset(lora_editor_value_labels, 0, sizeof(lora_editor_value_labels));
}

static void lora_editor_field_submit_cb(const char *text, void *user_data)
{
    lora_edit_field_t field =
        (lora_edit_field_t)(intptr_t)user_data;

    if(!text) {
        text = "";
    }

    switch(field) {
    case LORA_EDIT_NAME:
        snprintf(lora_editor_profile.name, sizeof(lora_editor_profile.name),
                 "%s", text);
        break;
    case LORA_EDIT_FREQ:
        lora_editor_profile.freq = (float)atof(text);
        break;
    case LORA_EDIT_BW:
        lora_editor_profile.bandwidth = (float)atof(text);
        break;
    case LORA_EDIT_POWER:
        lora_editor_profile.power = (int8_t)atoi(text);
        break;
    case LORA_EDIT_SF:
        lora_editor_profile.sf = (uint8_t)atoi(text);
        break;
    case LORA_EDIT_CR:
        lora_editor_profile.cr = (uint8_t)atoi(text);
        break;
    case LORA_EDIT_SYNC:
        lora_editor_profile.sync_word =
            lora_parse_sync_word(text, lora_editor_profile.sync_word);
        break;
    case LORA_EDIT_PREAMBLE:
        lora_editor_profile.preamble = (uint16_t)atoi(text);
        break;
    case LORA_EDIT_INTERVAL:
        lora_editor_profile.interval_ms = (uint32_t)atoi(text);
        break;
    default:
        break;
    }

    lora_sanitize_profile(&lora_editor_profile);
    lora_update_editor_values();
}

static void lora_editor_field_event_cb(lv_event_t *event)
{
    lora_edit_field_t field =
        (lora_edit_field_t)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t config;
    char value[96];

    lora_format_editor_value(field, value, sizeof(value));
    memset(&config, 0, sizeof(config));
    config.title = lora_editor_field_title(field);
    config.placeholder = lora_editor_field_hint(field);
    config.initial_text = value;
    config.password_mode = 0;
    config.max_length = field == LORA_EDIT_NAME ?
                        LORA_PROFILE_NAME_MAX - 1U : 16U;
    config.submit_cb = lora_editor_field_submit_cb;
    config.user_data = (void *)(intptr_t)field;
    ui_input_dialog_open(&config);
}

static lv_obj_t *lora_editor_row(lv_obj_t *parent, int y,
                                 lora_edit_field_t field)
{
    char value[96];
    int row_w = lora_panel_content_width(parent);
    int value_w;
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, row_w, 66);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x222A34), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x303C49), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2F3A46), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, lora_editor_field_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)field);

    lv_obj_t *title = ui_label(row, lora_editor_field_title(field),
                               &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(title, row_w / 2 - 24);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    ui_make_click_forwarder(title);

    lora_format_editor_value(field, value, sizeof(value));
    lv_obj_t *val = ui_label(row, value, &lv_font_montserrat_18, 0x9AA4AF);
    value_w = row_w / 2 - 24;
    if(value_w < 220) {
        value_w = 220;
    }
    lv_obj_set_width(val, value_w);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -12, 0);
    ui_make_click_forwarder(val);
    lora_editor_value_labels[field] = val;
    return row;
}

static void lora_editor_cancel_cb(lv_event_t *event)
{
    (void)event;
    lora_editor_close();
}

static void lora_editor_save_cb(lv_event_t *event)
{
    int target = lora_editor_source_index;

    (void)event;
    lora_sanitize_profile(&lora_editor_profile);
    lora_editor_profile.builtin = 0;

    if(target >= 0 && target < lora_builtin_profile_count) {
        target = -1;
    }
    if(target < 0 || target >= lora_profile_count) {
        if(lora_profile_count >= LORA_MAX_PROFILES) {
            lora_set_status("Profile storage full");
            lora_log("Profile save rejected: storage full");
            return;
        }
        target = lora_profile_count++;
    }

    lora_profiles[target] = lora_editor_profile;
    lora_active_profile = target;
    lora_save_profiles();
    lora_editor_close();
    lora_rebuild_factory_panel();
    lora_show_tab(0);
    lora_message_count = 0;
    if(lora_message_cont && lv_obj_is_valid(lora_message_cont)) {
        lv_obj_clean(lora_message_cont);
    }
    lora_enter_listen_mode();
}

static void lora_editor_delete_cb(lv_event_t *event)
{
    int index = lora_editor_source_index;

    (void)event;
    if(index < lora_builtin_profile_count || index >= lora_profile_count) {
        return;
    }

    for(int i = index; i + 1 < lora_profile_count; i++) {
        lora_profiles[i] = lora_profiles[i + 1];
    }
    lora_profile_count--;

    if(lora_active_profile == index || lora_active_profile >= lora_profile_count) {
        lora_active_profile = lora_profile_count > LORA_DEFAULT_PROFILE_INDEX ?
                              LORA_DEFAULT_PROFILE_INDEX : 0;
    } else if(lora_active_profile > index) {
        lora_active_profile--;
    }

    lora_save_profiles();
    lora_editor_close();
    lora_rebuild_factory_panel();
    lora_show_tab(1);
    lora_abort_radio_op();
    lora_apply_profile(lora_active_profile);
    lora_update_stats();
}

static void lora_open_profile_editor(int index)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *btn;
    int landscape = ui_is_landscape();
    int panel_x = landscape ? 64 : 24;
    int panel_y = landscape ? 56 : 74;
    int panel_w = landscape ? ui_screen_width() - 128 : 520;
    int panel_h = landscape ? ui_screen_height() - 96 : 1134;
    int inner_w;
    int row_y0 = landscape ? 82 : 88;
    int row_gap = landscape ? 58 : 76;
    int button_y;
    int button_gap = landscape ? 18 : 17;
    int button_w;

    lora_editor_close();
    lora_editor_source_index = index;

    if(index >= 0 && index < lora_profile_count) {
        lora_editor_profile = lora_profiles[index];
        if(lora_editor_profile.builtin) {
            char name[LORA_PROFILE_NAME_MAX];
            snprintf(name, sizeof(name), "%s Copy", lora_editor_profile.name);
            snprintf(lora_editor_profile.name,
                     sizeof(lora_editor_profile.name), "%s", name);
            lora_editor_profile.builtin = 0;
        }
    } else {
        int base = lora_active_profile >= 0 &&
                   lora_active_profile < lora_profile_count ?
                   lora_active_profile : 2;
        lora_editor_profile = lora_profiles[base];
        snprintf(lora_editor_profile.name, sizeof(lora_editor_profile.name),
                 "User %d", lora_profile_count - lora_builtin_profile_count + 1);
        lora_editor_profile.builtin = 0;
    }
    lora_sanitize_profile(&lora_editor_profile);

    lora_editor_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(lora_editor_overlay);
    lv_obj_align(lora_editor_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(lora_editor_overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(lora_editor_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lora_editor_overlay, 0, 0);
    lv_obj_set_style_pad_all(lora_editor_overlay, 0, 0);
    lv_obj_clear_flag(lora_editor_overlay, LV_OBJ_FLAG_SCROLLABLE);

    if(panel_w < 520) {
        panel_w = 520;
    }
    if(panel_h < 420) {
        panel_h = 420;
    }

    panel = ui_panel(lora_editor_overlay, panel_x, panel_y, panel_w, panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    inner_w = lora_panel_content_width(panel);

    title = ui_label(panel,
                     index < 0 ? "New Profile" : "Edit Profile",
                     &lv_font_montserrat_26, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    hint = ui_label(panel,
                    "Tap a row to edit. Factory profiles are copied when saved.",
                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(hint, inner_w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 42);

    for(int i = 0; i < LORA_EDIT_FIELD_COUNT; i++) {
        lora_editor_row(panel, row_y0 + i * row_gap, (lora_edit_field_t)i);
    }

    button_y = row_y0 + LORA_EDIT_FIELD_COUNT * row_gap + 22;
    button_w = (inner_w - button_gap * 2) / 3;
    if(button_w < 150) {
        button_w = 150;
    }

    btn = lora_button(panel, 0, button_y, button_w, 58, "Cancel", 0xF2F5F8);
    lv_obj_add_event_cb(btn, lora_editor_cancel_cb, LV_EVENT_CLICKED, NULL);
    if(index >= lora_builtin_profile_count && index < lora_profile_count) {
        lora_editor_delete_button =
            lora_button(panel, button_w + button_gap, button_y, button_w, 58,
                        "Delete", 0xEF4D5A);
        lv_obj_add_event_cb(lora_editor_delete_button, lora_editor_delete_cb,
                            LV_EVENT_CLICKED, NULL);
    }
    btn = lora_button(panel, (button_w + button_gap) * 2, button_y, button_w,
                      58, "Save", 0x25C281);
    lv_obj_add_event_cb(btn, lora_editor_save_cb, LV_EVENT_CLICKED, NULL);
    lora_update_editor_values();
}

static void lora_add_profile_event_cb(lv_event_t *event)
{
    (void)event;
    lora_open_profile_editor(-1);
}

static void lora_profile_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if(index < 0 || index >= lora_profile_count || lora_editor_overlay) {
        return;
    }

    if(code == LV_EVENT_LONG_PRESSED) {
        lora_open_profile_editor(index);
        return;
    }
    if(code != LV_EVENT_CLICKED) {
        return;
    }

    lora_active_profile = index;
    lora_message_count = 0;
    if(lora_message_cont && lv_obj_is_valid(lora_message_cont)) {
        lv_obj_clean(lora_message_cont);
    }
    lora_show_tab(0);
    lora_enter_listen_mode();
}

static void lora_session_event_cb(lv_event_t *event)
{
    lora_session_mode_t mode =
        (lora_session_mode_t)(intptr_t)lv_event_get_user_data(event);

    if(mode == LORA_SESSION_AUTO_TX) {
        lora_enter_auto_tx_mode();
    } else if(mode == LORA_SESSION_CARRIER) {
        lora_open_carrier_confirm();
    } else {
        lora_enter_listen_mode();
    }
}

static void lora_payload_mode_event_cb(lv_event_t *event)
{
    lora_payload_mode =
        (lora_payload_mode_t)(intptr_t)lv_event_get_user_data(event);
    lora_update_stats();
    lora_log("Payload mode %s", lora_payload_mode_text());
}

static void lora_show_tab(int tab)
{
    if(lora_factory_panel) {
        if(tab == 1) {
            lv_obj_clear_flag(lora_factory_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lora_factory_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(lora_chat_panel) {
        if(tab == 0) {
            lv_obj_clear_flag(lora_chat_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lora_chat_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(lora_log_panel) {
        if(tab == 2) {
            lv_obj_clear_flag(lora_log_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lora_log_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(lora_tab_factory) {
        lora_style_button_selected(lora_tab_factory, tab == 1, 0x7C3AED);
    }
    if(lora_tab_radio) {
        lora_style_button_selected(lora_tab_radio, tab == 0, 0x7C3AED);
    }
    if(lora_tab_log) {
        lora_style_button_selected(lora_tab_log, tab == 2, 0x7C3AED);
    }
    if(lora_add_button) {
        lora_set_hidden(lora_add_button, tab != 1);
    }
}

static void lora_tab_event_cb(lv_event_t *event)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(event);
    lora_show_tab(tab);
}

static void lora_create_status(lv_obj_t *body)
{
    int landscape = ui_is_landscape();
    int panel_w = landscape ? lora_landscape_left_w() : 520;
    int panel_h = landscape ? 258 : 220;
    lv_obj_t *panel = ui_panel(body, 24, landscape ? 24 : 12,
                               panel_w, panel_h);
    int inner_w = lora_panel_content_width(panel);
    int status_w = landscape ? inner_w : (inner_w > 700 ? 420 : 260);
    int stat_y = panel_h - 58;
    int tx_w = landscape ? 66 : 104;
    int rx_x = landscape ? 74 : 124;
    int rx_w = landscape ? 66 : 104;
    int auto_x = landscape ? 150 : 250;
    int auto_w = inner_w - auto_x;
    lv_obj_t *title = ui_label(panel, lora_active_chip_name(),
                               &lv_font_montserrat_26, 0xF2F5F8);
    lv_obj_t *pins = ui_label(panel,
                              landscape ?
                              "SPI 15/16/17  CS14  RST5\nBUSY19  IRQ20  LR2021 DIO11\nPWR44" :
                              "SPI io15/io16/io17  CS io14  RST io5  BUSY io19  IRQ io20  LR2021 DIO11  PWR io44",
                              &lv_font_montserrat_14, 0x9AA4AF);

    lora_chip_label = title;
    lv_obj_set_width(title, landscape ? inner_w : inner_w - status_w - 20);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lora_status_label = ui_label(panel, lora_status_text,
                                 &lv_font_montserrat_18, 0x25C281);
    lv_obj_align(lora_status_label,
                 landscape ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
                 0, landscape ? 36 : 4);
    lv_obj_set_width(lora_status_label, status_w);
    lv_label_set_long_mode(lora_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lora_status_label,
                                landscape ? LV_TEXT_ALIGN_LEFT :
                                LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_align(pins, LV_ALIGN_TOP_LEFT, 0, landscape ? 70 : 42);
    lv_obj_set_width(pins, inner_w);
    lv_label_set_long_mode(pins, LV_LABEL_LONG_WRAP);

    lora_profile_label = ui_label(panel, "", &lv_font_montserrat_16,
                                  0xF2F5F8);
    lv_obj_set_width(lora_profile_label, inner_w);
    lv_label_set_long_mode(lora_profile_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(lora_profile_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 144 : 88);

    lora_tx_label = ui_label(panel, "TX 0",
                             landscape ? &lv_font_montserrat_18 :
                             &lv_font_montserrat_22,
                             0x3DA5FF);
    lv_obj_set_width(lora_tx_label, tx_w);
    lv_label_set_long_mode(lora_tx_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lora_tx_label, 0, stat_y);
    lora_rx_label = ui_label(panel, "RX 0",
                             landscape ? &lv_font_montserrat_18 :
                             &lv_font_montserrat_22,
                             0x25C281);
    lv_obj_set_width(lora_rx_label, rx_w);
    lv_label_set_long_mode(lora_rx_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lora_rx_label, rx_x, stat_y);
    lora_auto_label = ui_label(panel, "Auto TX off",
                               landscape ? &lv_font_montserrat_16 :
                               &lv_font_montserrat_18,
                               0xF5A524);
    if(auto_w < 84) {
        auto_w = 84;
    }
    lv_obj_set_width(lora_auto_label, auto_w);
    lv_label_set_long_mode(lora_auto_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lora_auto_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lora_auto_label, auto_x, stat_y + 2);
}

static lv_obj_t *lora_create_profile_card(lv_obj_t *parent, int y, int index)
{
    char summary[160];
    const lora_profile_t *profile = &lora_profiles[index];
    int card_w = lora_panel_content_width(parent);
    int tag_w = 92;
    int name_w;
    lv_obj_t *card = lv_obj_create(parent);

    if(card_w < 360) {
        card_w = 360;
    }
    name_w = card_w - tag_w - 44;
    if(name_w < 180) {
        name_w = card_w - 28;
    }

    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, card_w, 94);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x222A34), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x303C49), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2F3A46), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(card, 4);
    lv_obj_add_event_cb(card, lora_profile_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    lv_obj_add_event_cb(card, lora_profile_event_cb, LV_EVENT_LONG_PRESSED,
                        (void *)(intptr_t)index);

    lv_obj_t *name = ui_label(card, profile->name, &lv_font_montserrat_20,
                              0xF2F5F8);
    lv_obj_set_width(name, name_w);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, 12, 10);
    ui_make_click_forwarder(name);

    lv_obj_t *tag = ui_label(card, profile->builtin ? "Factory" : "User",
                              &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(tag, tag_w);
    lv_label_set_long_mode(tag, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(tag,
                                lv_color_hex(profile->builtin ? 0xF5A524 :
                                             0x3DA5FF), 0);
    lv_obj_set_pos(tag, card_w - tag_w - 12, 12);
    ui_make_click_forwarder(tag);

    lora_format_profile_summary(profile, summary, sizeof(summary));
    lv_obj_t *sub = ui_label(card, summary, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(sub, card_w - 24);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(sub, 12, 52);
    ui_make_click_forwarder(sub);
    return card;
}

static void lora_rebuild_factory_panel(void)
{
    if(!lora_factory_panel || !lv_obj_is_valid(lora_factory_panel)) {
        return;
    }

    memset(lora_profile_buttons, 0, sizeof(lora_profile_buttons));
    lv_obj_clean(lora_factory_panel);

    lv_obj_t *title = ui_label(lora_factory_panel, "Profiles",
                               &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = ui_label(lora_factory_panel,
                              "Tap to open. Long press to edit.",
                              &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(hint, lora_panel_content_width(lora_factory_panel));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 34);

    for(int i = 0; i < lora_profile_count; i++) {
        lora_profile_buttons[i] =
            lora_create_profile_card(lora_factory_panel, 78 + i * 108, i);
    }
    lora_update_session_ui();
}

static void lora_create_factory(lv_obj_t *body)
{
    lora_factory_panel = ui_panel(body,
                                  ui_is_landscape() ?
                                  lora_landscape_right_x() : 24,
                                  ui_is_landscape() ? 24 : 320,
                                  ui_is_landscape() ?
                                  lora_landscape_right_w() : 520,
                                  ui_is_landscape() ?
                                  lora_landscape_panel_h() : 744);
    lv_obj_set_style_bg_color(lora_factory_panel, lv_color_hex(0x101418), 0);
    lv_obj_add_flag(lora_factory_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(lora_factory_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lora_factory_panel, LV_SCROLLBAR_MODE_OFF);
    lora_rebuild_factory_panel();
}

static void lora_create_add_button(lv_obj_t *scr)
{
    int landscape = ui_is_landscape();
    int size = landscape ? 58 : 64;

    lora_add_button = lv_obj_create(scr);
    lv_obj_set_size(lora_add_button, size, size);
    if(landscape) {
        int x = lora_landscape_right_x() + lora_landscape_right_w() - size - 30;
        int y = ui_page_top_y(144) + 24 + lora_landscape_panel_h() - size - 30;

        if(x > ui_screen_width() - size - 36) {
            x = ui_screen_width() - size - 36;
        }
        if(y > ui_screen_height() - size - 36) {
            y = ui_screen_height() - size - 36;
        }
        lv_obj_set_pos(lora_add_button, x, y);
    } else {
        lv_obj_align(lora_add_button, LV_ALIGN_BOTTOM_RIGHT, -24, -24);
    }
    lv_obj_set_style_radius(lora_add_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lora_add_button, lv_color_hex(0x7C3AED), 0);
    lv_obj_set_style_bg_color(lora_add_button, lv_color_hex(0x6D28D9),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(lora_add_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lora_add_button, 0, 0);
    lv_obj_clear_flag(lora_add_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lora_add_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lora_add_button, 8);
    lv_obj_add_event_cb(lora_add_button, lora_add_profile_event_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = ui_label(lora_add_button, "+", &lv_font_montserrat_28,
                               0xFFFFFF);
    lv_obj_center(label);
    ui_make_click_forwarder(label);
}

static void lora_create_chat(lv_obj_t *body)
{
    int landscape = ui_is_landscape();
    int panel_w = landscape ? lora_landscape_right_w() : 520;
    int panel_h;
    int mode_y;
    int session_y;
    int payload_y;
    int payload_btn_y;
    int msg_y;
    int msg_h;
    int session_gap = landscape ? 12 : 14;
    int label_w;
    int controls_x;
    int session_w;
    int payload_label_w;
    int payload_x;
    int payload_w;

    lora_chat_panel = ui_panel(body,
                               landscape ? lora_landscape_right_x() : 24,
                               landscape ? 24 : 320,
                               panel_w,
                               landscape ? lora_landscape_panel_h() : 744);
    panel_h = lv_obj_get_height(lora_chat_panel);
    int inner_w = lora_panel_content_width(lora_chat_panel);
    lv_obj_t *title = ui_label(lora_chat_panel, "RF Test", &lv_font_montserrat_22,
                               0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lora_payload_label = ui_label(lora_chat_panel, "", &lv_font_montserrat_18,
                                  0xF2F5F8);
    lv_obj_set_width(lora_payload_label, inner_w);
    lv_obj_set_height(lora_payload_label, landscape ? 34 : 58);
    lv_label_set_long_mode(lora_payload_label, LV_LABEL_LONG_DOT);
    lv_obj_align(lora_payload_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 34 : 38);

    if(landscape) {
        label_w = 84;
        controls_x = label_w + 12;
        mode_y = 66;
        session_y = 54;
        payload_y = 120;
        payload_btn_y = 108;
        msg_y = 164;
        session_w = (inner_w - controls_x -
                     session_gap * (LORA_SESSION_BUTTON_COUNT - 1)) /
                    LORA_SESSION_BUTTON_COUNT;
        payload_label_w = label_w;
        payload_x = controls_x;
        payload_w = (inner_w - payload_x - session_gap) / 2;
    } else {
        label_w = 58;
        controls_x = 70;
        mode_y = 126;
        session_y = 112;
        payload_y = 188;
        payload_btn_y = 174;
        msg_y = 234;
        session_w = (inner_w - controls_x -
                     session_gap * (LORA_SESSION_BUTTON_COUNT - 1)) /
                    LORA_SESSION_BUTTON_COUNT;
        payload_label_w = 90;
        payload_x = 104;
        payload_w = 178;
    }
    msg_h = panel_h - msg_y - 4;
    if(msg_h < 118) {
        msg_h = 118;
    }

    lv_obj_t *mode = ui_label(lora_chat_panel, "Mode", &lv_font_montserrat_16,
                              0x9AA4AF);
    lv_obj_set_width(mode, label_w);
    lv_label_set_long_mode(mode, LV_LABEL_LONG_DOT);
    lv_obj_align(mode, LV_ALIGN_TOP_LEFT, 0, mode_y);
    lora_session_buttons[LORA_SESSION_LISTEN] =
        lora_button(lora_chat_panel, controls_x, session_y,
                    session_w, 46, "Listen", 0x25C281);
    lora_session_buttons[LORA_SESSION_AUTO_TX] =
        lora_button(lora_chat_panel,
                    controls_x + session_w + session_gap,
                    session_y, session_w, 46, "Auto TX",
                    0xF5A524);
    lora_session_buttons[LORA_SESSION_CARRIER] =
        lora_button(lora_chat_panel,
                    controls_x + (session_w + session_gap) * 2,
                    session_y, session_w, 46, "Carrier",
                    0xEF4D5A);
    for(int i = 0; i < LORA_SESSION_BUTTON_COUNT; i++) {
        lv_obj_add_event_cb(lora_session_buttons[i], lora_session_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lora_payload_row_label = ui_label(lora_chat_panel, "Payload",
                                      &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(lora_payload_row_label, payload_label_w);
    lv_label_set_long_mode(lora_payload_row_label, LV_LABEL_LONG_DOT);
    lv_obj_align(lora_payload_row_label, LV_ALIGN_TOP_LEFT, 0, payload_y);
    lora_payload_buttons[LORA_PAYLOAD_COUNTER] =
        lora_button(lora_chat_panel, payload_x, payload_btn_y,
                    payload_w, 46, "Counter", 0xF2F5F8);
    lora_payload_buttons[LORA_PAYLOAD_FIXED] =
        lora_button(lora_chat_panel,
                    payload_x + payload_w + session_gap, payload_btn_y,
                    payload_w, 46, "Fixed", 0xF2F5F8);
    for(int i = 0; i < 2; i++) {
        lv_obj_add_event_cb(lora_payload_buttons[i], lora_payload_mode_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lora_message_page = lv_obj_create(lora_chat_panel);
    lv_obj_set_pos(lora_message_page, 0, msg_y);
    lv_obj_set_size(lora_message_page, inner_w, msg_h);
    lv_obj_set_style_bg_color(lora_message_page, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(lora_message_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lora_message_page, 1, 0);
    lv_obj_set_style_border_color(lora_message_page, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_radius(lora_message_page, 8, 0);
    lv_obj_set_style_pad_all(lora_message_page, 6, 0);
    lv_obj_set_scroll_dir(lora_message_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lora_message_page, LV_SCROLLBAR_MODE_OFF);

    lora_message_cont = lv_obj_create(lora_message_page);
    lv_obj_set_size(lora_message_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(lora_message_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lora_message_cont, 0, 0);
    lv_obj_set_style_pad_all(lora_message_cont, 0, 0);
    lv_obj_set_style_pad_row(lora_message_cont, 4, 0);
    lv_obj_set_flex_flow(lora_message_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(lora_message_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(lora_chat_panel, LV_OBJ_FLAG_HIDDEN);
}

static void lora_create_log(lv_obj_t *body)
{
    lora_log_panel = ui_panel(body,
                              ui_is_landscape() ?
                              lora_landscape_right_x() : 24,
                              ui_is_landscape() ? 24 : 320,
                              ui_is_landscape() ?
                              lora_landscape_right_w() : 520,
                              ui_is_landscape() ?
                              lora_landscape_panel_h() : 744);
    lv_obj_t *title = ui_label(lora_log_panel, "Event log", &lv_font_montserrat_22,
                               0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lora_log_label = ui_label(lora_log_panel, "", &lv_font_montserrat_16, 0xC9D3DF);
    lv_obj_set_width(lora_log_label, lora_panel_content_width(lora_log_panel));
    lv_label_set_long_mode(lora_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(lora_log_label, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_add_flag(lora_log_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(lora_log_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lora_log_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(lora_log_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_lora_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    int landscape = ui_is_landscape();
    int left_w = landscape ? lora_landscape_left_w() : 520;

    body = ui_page_body(scr, 144);
    if(landscape) {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    }

    lora_load_profiles();
    lora_create_status(body);

    lora_tab_factory = lora_button(body, 24, landscape ? 302 : 254,
                                  landscape ? left_w : 160, 56, "Factory",
                                  0xF2F5F8);
    lv_obj_add_event_cb(lora_tab_factory, lora_tab_event_cb, LV_EVENT_CLICKED,
                        (void *)1);
    lora_tab_radio = lora_button(body, landscape ? 24 : 204,
                                 landscape ? 366 : 254,
                                 landscape ? left_w : 160, 56, "RF Test",
                                 0xF2F5F8);
    lv_obj_add_event_cb(lora_tab_radio, lora_tab_event_cb, LV_EVENT_CLICKED,
                        (void *)0);
    lora_tab_log = lora_button(body, landscape ? 24 : 384,
                               landscape ? 430 : 254,
                               landscape ? left_w : 160, 56, "Log",
                               0xF2F5F8);
    lv_obj_add_event_cb(lora_tab_log, lora_tab_event_cb, LV_EVENT_CLICKED,
                        (void *)2);

    lora_create_factory(body);
    lora_create_chat(body);
    lora_create_log(body);
    lora_show_tab(1);
    lora_update_stats();

    ui_create_header(scr, "LoRa");
    lora_create_add_button(scr);
    lora_log("UI enter pins: sclk=%u mosi=%u miso=%u cs=%u rst=%u busy=%u irq_gpio=%u lr2021_irq_dio=%u power=%u",
             LORA_PIN_SCLK, LORA_PIN_MOSI, LORA_PIN_MISO, LORA_PIN_CS,
             LORA_PIN_RST, LORA_PIN_BUSY, LORA_PIN_DIO1,
             LORA_LR2021_IRQ_DIO_NUM, LORA_PIN_POWER);

    if(!lora_timer) {
        lora_timer = lv_timer_create(lora_timer_cb, LORA_UI_TICK_MS, NULL);
    }
    if(lora_startup_timer) {
        lv_timer_delete(lora_startup_timer);
        lora_startup_timer = NULL;
    }
    lora_set_status("Radio start queued");
    lora_startup_timer =
        lv_timer_create(lora_startup_timer_cb, LORA_STARTUP_DELAY_MS, NULL);
    if(lora_startup_timer) {
        lv_timer_set_repeat_count(lora_startup_timer, 1);
    } else {
        lora_log("Startup timer allocation failed; starting radio inline");
        lora_apply_profile(lora_active_profile);
    }
}

typedef struct {
    const char *name;
    const LoRaWANBand_t *band;
    float rf_freq_mhz;
} lorawan_region_t;

typedef struct {
    const lorawan_region_t *region;
    uint8_t sub_band;
    uint64_t join_eui;
    uint64_t dev_eui;
    uint8_t app_key[16];
    uint8_t nwk_key[16];
    int has_dev_eui;
    int has_app_key;
    int has_nwk_key;
    uint32_t interval_s;
    uint8_t fport;
    int confirmed;
    int adr;
    uint8_t datarate;
    int duty_cycle;
    uint32_t duty_cycle_ms_per_hour;
    int dwell_time;
    uint32_t dwell_time_ms;
    int has_tx_power;
    int8_t tx_power;
    int simulate_temperature;
} lorawan_config_t;

static const lorawan_region_t lorawan_regions[] = {
    {"EU868", &EU868, 868.1f},
    {"US915", &US915, 915.0f},
    {"AU915", &AU915, 915.0f},
    {"EU433", &EU433, 433.0f},
    {"CN470", &CN470, 470.0f},
    {"AS923", &AS923, 923.2f},
    {"AS923_2", &AS923_2, 923.2f},
    {"AS923_3", &AS923_3, 923.2f},
    {"AS923_4", &AS923_4, 923.2f},
    {"KR920", &KR920, 920.0f},
    {"IN865", &IN865, 865.0f},
};

#define LORAWAN_REGION_COUNT \
    (sizeof(lorawan_regions) / sizeof(lorawan_regions[0]))

typedef enum {
    LORAWAN_VIEW_HOME = 0,
    LORAWAN_VIEW_LOAD,
    LORAWAN_VIEW_EDIT,
} lorawan_view_t;

static lv_obj_t *lorawan_status_label;
static lv_obj_t *lorawan_radio_label;
static lv_obj_t *lorawan_config_label;
static lv_obj_t *lorawan_profile_label;
static lv_obj_t *lorawan_region_label;
static lv_obj_t *lorawan_join_label;
static lv_obj_t *lorawan_devaddr_label;
static lv_obj_t *lorawan_fcnt_label;
static lv_obj_t *lorawan_payload_label;
static lv_obj_t *lorawan_downlink_label;
static lv_obj_t *lorawan_region_value_label;
static lv_obj_t *lorawan_join_eui_value_label;
static lv_obj_t *lorawan_dev_eui_value_label;
static lv_obj_t *lorawan_app_key_value_label;
static lv_obj_t *lorawan_nwk_key_value_label;
static lv_obj_t *lorawan_subband_value_label;
static lv_obj_t *lorawan_interval_value_label;
static lv_obj_t *lorawan_fport_value_label;
static lv_obj_t *lorawan_confirmed_switch;
static lv_obj_t *lorawan_adr_switch;
static lv_obj_t *lorawan_sim_temp_switch;
static lv_obj_t *lorawan_region_buttons[LORAWAN_REGION_COUNT];
static LoRaWANNode *lorawan_node;
static lorawan_config_t lorawan_config;
static int lorawan_config_loaded;
static int lorawan_config_valid;
static int lorawan_config_dirty;
static int lorawan_joined;
static lorawan_view_t lorawan_view = LORAWAN_VIEW_HOME;
static char lorawan_selected_file[LORAWAN_FILE_MAX];
static char lorawan_selected_path[LORAWAN_PATH_MAX];
static char lorawan_profile_files[LORAWAN_PROFILE_MAX][LORAWAN_FILE_MAX];
static int lorawan_profile_count;

static void lorawan_reset_node(void);
static void lorawan_update_labels(void);

static int lorawan_hex_value(char c)
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

static int lorawan_parse_hex_bytes(const char *text, uint8_t *out,
                                   size_t expected)
{
    size_t count = 0;
    int high = -1;

    if(!text || !out) {
        return 0;
    }
    for(size_t i = 0; text[i]; i++) {
        if(text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            i++;
            continue;
        }
        int value = lorawan_hex_value(text[i]);
        if(value < 0) {
            continue;
        }
        if(high < 0) {
            high = value;
        } else {
            if(count >= expected) {
                return 0;
            }
            out[count++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    return high < 0 && count == expected;
}

static int lorawan_parse_eui(const char *text, uint64_t *out)
{
    uint8_t bytes[8];
    uint64_t value = 0;

    if(!out || !lorawan_parse_hex_bytes(text, bytes, sizeof(bytes))) {
        return 0;
    }
    for(size_t i = 0; i < sizeof(bytes); i++) {
        value = (value << 8) | bytes[i];
    }
    *out = value;
    return 1;
}

static void lorawan_format_eui(uint64_t value, char *out, size_t out_len)
{
    if(!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%016llX", (unsigned long long)value);
}

static void lorawan_format_key(const uint8_t *key, char *out, size_t out_len)
{
    size_t pos = 0;

    if(!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if(!key) {
        return;
    }
    for(size_t i = 0; i < 16 && pos + 3 < out_len; i++) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X", key[i]);
    }
}

static void lorawan_format_key_short(const uint8_t *key, int valid,
                                     char *out, size_t out_len)
{
    char full[40];

    if(!out || out_len == 0) {
        return;
    }
    if(!valid || !key) {
        snprintf(out, out_len, "--");
        return;
    }
    lorawan_format_key(key, full, sizeof(full));
    snprintf(out, out_len, "%.8s...%.8s", full, full + 24);
}

static int lorawan_parse_bool(const char *text, int fallback)
{
    if(!text) {
        return fallback;
    }
    if(!strcasecmp(text, "1") || !strcasecmp(text, "true") ||
       !strcasecmp(text, "yes") || !strcasecmp(text, "on")) {
        return 1;
    }
    if(!strcasecmp(text, "0") || !strcasecmp(text, "false") ||
       !strcasecmp(text, "no") || !strcasecmp(text, "off")) {
        return 0;
    }
    return fallback;
}

static const lorawan_region_t *lorawan_find_region(const char *name)
{
    if(!name || !name[0]) {
        return &lorawan_regions[0];
    }
    for(size_t i = 0; i < sizeof(lorawan_regions) / sizeof(lorawan_regions[0]); i++) {
        if(!strcasecmp(name, lorawan_regions[i].name)) {
            return &lorawan_regions[i];
        }
    }
    return NULL;
}

static void lorawan_config_defaults(void)
{
    memset(&lorawan_config, 0, sizeof(lorawan_config));
    lorawan_config.region = &lorawan_regions[0];
    lorawan_config.interval_s = 300;
    lorawan_config.fport = 10;
    lorawan_config.confirmed = 0;
    lorawan_config.adr = 1;
    lorawan_config.datarate = 5;
    lorawan_config.duty_cycle = 1;
    lorawan_config.duty_cycle_ms_per_hour = 1250;
    lorawan_config.dwell_time = 1;
    lorawan_config.dwell_time_ms = 400;
    lorawan_config.tx_power = 14;
    lorawan_config.simulate_temperature = 0;
}

static void lorawan_set_status(const char *fmt, ...)
{
    char text[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if(lorawan_status_label && lv_obj_is_valid(lorawan_status_label)) {
        lv_label_set_text(lorawan_status_label, text);
    }
    lora_log("LoRaWAN %s", text);
}

static int lorawan_write_template(void)
{
    FILE *fp;

    mkdir(LORAWAN_CONFIG_DIR, 0755);
    fp = fopen(LORAWAN_CONFIG_PATH, "w");
    if(!fp) {
        return 0;
    }
    fprintf(fp, "# K230 LoRaWAN OTAA config\n");
    fprintf(fp, "# Supported regions: EU868,US915,AU915,EU433,CN470,AS923,AS923_2,AS923_3,AS923_4,KR920,IN865\n");
    fprintf(fp, "region=EU868\n");
    fprintf(fp, "sub_band=0\n");
    fprintf(fp, "join_eui=0000000000000000\n");
    fprintf(fp, "dev_eui=0011223344556677\n");
    fprintf(fp, "app_key=00112233445566778899AABBCCDDEEFF\n");
    fprintf(fp, "nwk_key=00112233445566778899AABBCCDDEEFF\n");
    fprintf(fp, "uplink_interval_s=300\n");
    fprintf(fp, "fport=10\n");
    fprintf(fp, "confirmed=false\n");
    fprintf(fp, "adr=true\n");
    fprintf(fp, "datarate=5\n");
    fprintf(fp, "duty_cycle=true\n");
    fprintf(fp, "duty_cycle_ms_per_hour=1250\n");
    fprintf(fp, "dwell_time=true\n");
    fprintf(fp, "dwell_time_ms=400\n");
    fprintf(fp, "simulate_temperature=false\n");
    fprintf(fp, "# Optional: tx_power=14\n");
    fclose(fp);
    return 1;
}

static int lorawan_file_exists(const char *path)
{
    return path && path[0] && access(path, F_OK) == 0;
}

static const char *lorawan_basename(const char *path)
{
    const char *slash;

    if(!path) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int lorawan_has_suffix(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if(!text || !suffix) {
        return 0;
    }
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcasecmp(text + text_len - suffix_len, suffix) == 0;
}

static void lorawan_set_selected_profile(const char *path)
{
    if(!path || !path[0]) {
        lorawan_selected_file[0] = '\0';
        lorawan_selected_path[0] = '\0';
        return;
    }
    snprintf(lorawan_selected_path, sizeof(lorawan_selected_path), "%s", path);
    snprintf(lorawan_selected_file, sizeof(lorawan_selected_file), "%s",
             lorawan_basename(path));
}

static void lorawan_build_profile_path(const char *file, char *out,
                                       size_t out_len)
{
    if(!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%s/%s", LORAWAN_CONFIG_DIR,
             file && file[0] ? file : "lora_wan_0.json");
}

static void lorawan_sanitize_profile_file(const char *input, char *out,
                                          size_t out_len)
{
    char temp[LORAWAN_FILE_MAX];
    size_t wr = 0;

    if(!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    snprintf(temp, sizeof(temp), "%s", input ? input : "");
    ui_trim_text(temp);
    for(size_t i = 0; temp[i] && wr + 1 < out_len; i++) {
        unsigned char c = (unsigned char)temp[i];

        if(c == '/' || c == '\\') {
            continue;
        }
        if(isalnum(c) || c == '_' || c == '-' || c == '.') {
            out[wr++] = (char)c;
        }
    }
    out[wr] = '\0';
    if(!out[0] || strstr(out, "..")) {
        snprintf(out, out_len, "%s0%s", LORAWAN_PROFILE_PREFIX,
                 LORAWAN_PROFILE_EXT);
    }
    if(!lorawan_has_suffix(out, LORAWAN_PROFILE_EXT)) {
        size_t len = strlen(out);
        if(len + strlen(LORAWAN_PROFILE_EXT) + 1 < out_len) {
            strcat(out, LORAWAN_PROFILE_EXT);
        }
    }
}

static void lorawan_default_profile_name(char *out, size_t out_len)
{
    char file[LORAWAN_FILE_MAX];
    char path[LORAWAN_PATH_MAX];

    if(!out || out_len == 0) {
        return;
    }
    for(int i = 0; i < 1000; i++) {
        snprintf(file, sizeof(file), "%s%d%s", LORAWAN_PROFILE_PREFIX, i,
                 LORAWAN_PROFILE_EXT);
        lorawan_build_profile_path(file, path, sizeof(path));
        if(!lorawan_file_exists(path)) {
            snprintf(out, out_len, "%s", file);
            return;
        }
    }
    snprintf(out, out_len, "%s", "lora_wan_backup.json");
}

static int lorawan_scan_profiles(void)
{
    DIR *dir;
    struct dirent *entry;

    lorawan_profile_count = 0;
    mkdir(LORAWAN_CONFIG_DIR, 0755);
    dir = opendir(LORAWAN_CONFIG_DIR);
    if(!dir) {
        return 0;
    }
    while((entry = readdir(dir)) != NULL &&
          lorawan_profile_count < LORAWAN_PROFILE_MAX) {
        const char *name = entry->d_name;

        if(name[0] == '.') {
            continue;
        }
        if(!lorawan_has_suffix(name, LORAWAN_PROFILE_EXT) &&
           !lorawan_has_suffix(name, ".conf")) {
            continue;
        }
        snprintf(lorawan_profile_files[lorawan_profile_count],
                 sizeof(lorawan_profile_files[0]), "%s", name);
        lorawan_profile_count++;
    }
    closedir(dir);

    for(int i = 1; i < lorawan_profile_count; i++) {
        char value[LORAWAN_FILE_MAX];
        int j = i - 1;

        snprintf(value, sizeof(value), "%s", lorawan_profile_files[i]);
        while(j >= 0 && strcasecmp(lorawan_profile_files[j], value) > 0) {
            snprintf(lorawan_profile_files[j + 1],
                     sizeof(lorawan_profile_files[0]), "%s",
                     lorawan_profile_files[j]);
            j--;
        }
        snprintf(lorawan_profile_files[j + 1],
                 sizeof(lorawan_profile_files[0]), "%s", value);
    }
    return lorawan_profile_count;
}

static int lorawan_validate_config(int report_status)
{
    if(!lorawan_config.region) {
        if(report_status) {
            lorawan_set_status("Region invalid");
        }
        return 0;
    }
    if(!lorawan_config.has_dev_eui) {
        if(report_status) {
            lorawan_set_status("DevEUI must be 16 hex digits");
        }
        return 0;
    }
    if(!lorawan_config.has_app_key) {
        if(report_status) {
            lorawan_set_status("AppKey must be 32 hex digits");
        }
        return 0;
    }
    if(!lorawan_config.has_nwk_key) {
        if(report_status) {
            lorawan_set_status("NwkKey must be 32 hex digits");
        }
        return 0;
    }
    if(lorawan_config.fport < 1 || lorawan_config.fport > 223) {
        if(report_status) {
            lorawan_set_status("FPort invalid");
        }
        return 0;
    }
    if(lorawan_config.interval_s < 60U) {
        if(report_status) {
            lorawan_set_status("Interval must be at least 60s");
        }
        return 0;
    }
    return 1;
}

static int lorawan_save_config_conf(const char *path)
{
    FILE *fp;
    char join_eui[24];
    char dev_eui[24];
    char app_key[40];
    char nwk_key[40];

    mkdir(LORAWAN_CONFIG_DIR, 0755);
    fp = fopen(path ? path : LORAWAN_CONFIG_PATH, "w");
    if(!fp) {
        lorawan_set_status("Config save failed");
        return 0;
    }

    lorawan_format_eui(lorawan_config.join_eui, join_eui, sizeof(join_eui));
    lorawan_format_eui(lorawan_config.dev_eui, dev_eui, sizeof(dev_eui));
    lorawan_format_key(lorawan_config.app_key, app_key, sizeof(app_key));
    lorawan_format_key(lorawan_config.nwk_key, nwk_key, sizeof(nwk_key));

    fprintf(fp, "# K230 LoRaWAN OTAA config\n");
    fprintf(fp, "# Generated by k230_phone_ui\n");
    fprintf(fp, "region=%s\n", lorawan_config.region ?
            lorawan_config.region->name : lorawan_regions[0].name);
    fprintf(fp, "sub_band=%u\n", lorawan_config.sub_band);
    fprintf(fp, "join_eui=%s\n", join_eui);
    fprintf(fp, "dev_eui=%s\n", dev_eui);
    fprintf(fp, "app_key=%s\n", app_key);
    fprintf(fp, "nwk_key=%s\n", nwk_key);
    fprintf(fp, "uplink_interval_s=%lu\n",
            (unsigned long)lorawan_config.interval_s);
    fprintf(fp, "fport=%u\n", lorawan_config.fport);
    fprintf(fp, "confirmed=%s\n", lorawan_config.confirmed ? "true" : "false");
    fprintf(fp, "adr=%s\n", lorawan_config.adr ? "true" : "false");
    fprintf(fp, "datarate=%u\n", lorawan_config.datarate);
    fprintf(fp, "duty_cycle=%s\n",
            lorawan_config.duty_cycle ? "true" : "false");
    fprintf(fp, "duty_cycle_ms_per_hour=%lu\n",
            (unsigned long)lorawan_config.duty_cycle_ms_per_hour);
    fprintf(fp, "dwell_time=%s\n",
            lorawan_config.dwell_time ? "true" : "false");
    fprintf(fp, "dwell_time_ms=%lu\n",
            (unsigned long)lorawan_config.dwell_time_ms);
    fprintf(fp, "simulate_temperature=%s\n",
            lorawan_config.simulate_temperature ? "true" : "false");
    if(lorawan_config.has_tx_power) {
        fprintf(fp, "tx_power=%d\n", lorawan_config.tx_power);
    }
    fclose(fp);

    lorawan_config_loaded = 1;
    lorawan_config_valid = lorawan_validate_config(0);
    lorawan_config_dirty = 0;
    lorawan_set_selected_profile(path ? path : LORAWAN_CONFIG_PATH);
    lorawan_set_status(lorawan_config_valid ? "Config saved" :
                       "Config incomplete");
    return 1;
}

static int lorawan_save_config_json(const char *path)
{
    FILE *fp;
    char join_eui[24];
    char dev_eui[24];
    char app_key[40];
    char nwk_key[40];

    if(!lorawan_validate_config(1)) {
        lorawan_config_valid = 0;
        return 0;
    }

    mkdir(LORAWAN_CONFIG_DIR, 0755);
    fp = fopen(path, "w");
    if(!fp) {
        lorawan_set_status("Config save failed");
        return 0;
    }

    lorawan_format_eui(lorawan_config.join_eui, join_eui, sizeof(join_eui));
    lorawan_format_eui(lorawan_config.dev_eui, dev_eui, sizeof(dev_eui));
    lorawan_format_key(lorawan_config.app_key, app_key, sizeof(app_key));
    lorawan_format_key(lorawan_config.nwk_key, nwk_key, sizeof(nwk_key));

    fprintf(fp, "{\n");
    fprintf(fp, "  \"version\": 1,\n");
    fprintf(fp, "  \"region\": \"%s\",\n",
            lorawan_config.region ? lorawan_config.region->name :
            lorawan_regions[0].name);
    fprintf(fp, "  \"sub_band\": %u,\n", lorawan_config.sub_band);
    fprintf(fp, "  \"join_eui\": \"%s\",\n", join_eui);
    fprintf(fp, "  \"dev_eui\": \"%s\",\n", dev_eui);
    fprintf(fp, "  \"app_key\": \"%s\",\n", app_key);
    fprintf(fp, "  \"nwk_key\": \"%s\",\n", nwk_key);
    fprintf(fp, "  \"uplink_interval_s\": %lu,\n",
            (unsigned long)lorawan_config.interval_s);
    fprintf(fp, "  \"fport\": %u,\n", lorawan_config.fport);
    fprintf(fp, "  \"confirmed\": %s,\n",
            lorawan_config.confirmed ? "true" : "false");
    fprintf(fp, "  \"adr\": %s,\n", lorawan_config.adr ? "true" : "false");
    fprintf(fp, "  \"datarate\": %u,\n", lorawan_config.datarate);
    fprintf(fp, "  \"duty_cycle\": %s,\n",
            lorawan_config.duty_cycle ? "true" : "false");
    fprintf(fp, "  \"duty_cycle_ms_per_hour\": %lu,\n",
            (unsigned long)lorawan_config.duty_cycle_ms_per_hour);
    fprintf(fp, "  \"dwell_time\": %s,\n",
            lorawan_config.dwell_time ? "true" : "false");
    fprintf(fp, "  \"dwell_time_ms\": %lu,\n",
            (unsigned long)lorawan_config.dwell_time_ms);
    fprintf(fp, "  \"simulate_temperature\": %s",
            lorawan_config.simulate_temperature ? "true" : "false");
    if(lorawan_config.has_tx_power) {
        fprintf(fp, ",\n  \"tx_power\": %d\n", lorawan_config.tx_power);
    } else {
        fprintf(fp, "\n");
    }
    fprintf(fp, "}\n");
    fclose(fp);

    lorawan_config_loaded = 1;
    lorawan_config_valid = 1;
    lorawan_config_dirty = 0;
    lorawan_set_selected_profile(path);
    lorawan_set_status("Profile saved: %s", lorawan_selected_file);
    return 1;
}

static const char *lorawan_json_key(const char *json, const char *key)
{
    char needle[64];
    const char *pos;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    pos = strstr(json, needle);
    if(!pos) {
        return NULL;
    }
    pos += strlen(needle);
    while(*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if(*pos != ':') {
        return NULL;
    }
    pos++;
    while(*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    return pos;
}

static int lorawan_json_get_string(const char *json, const char *key,
                                   char *out, size_t out_len)
{
    const char *pos = lorawan_json_key(json, key);
    size_t wr = 0;

    if(!pos || *pos != '"' || !out || out_len == 0) {
        return 0;
    }
    pos++;
    while(*pos && *pos != '"' && wr + 1 < out_len) {
        if(*pos == '\\' && pos[1]) {
            pos++;
        }
        out[wr++] = *pos++;
    }
    out[wr] = '\0';
    return *pos == '"';
}

static int lorawan_json_get_int(const char *json, const char *key,
                                int *out)
{
    const char *pos = lorawan_json_key(json, key);

    if(!pos || !out) {
        return 0;
    }
    *out = atoi(pos);
    return 1;
}

static int lorawan_json_get_bool(const char *json, const char *key,
                                 int *out)
{
    const char *pos = lorawan_json_key(json, key);

    if(!pos || !out) {
        return 0;
    }
    if(!strncasecmp(pos, "true", 4) || *pos == '1') {
        *out = 1;
        return 1;
    }
    if(!strncasecmp(pos, "false", 5) || *pos == '0') {
        *out = 0;
        return 1;
    }
    return 0;
}

static int lorawan_load_json_path(const char *path)
{
    FILE *fp;
    char json[4096];
    size_t got;
    char value[96];
    int number;

    fp = fopen(path, "r");
    if(!fp) {
        return 0;
    }
    got = fread(json, 1, sizeof(json) - 1, fp);
    fclose(fp);
    json[got] = '\0';

    lorawan_config_defaults();
    if(lorawan_json_get_string(json, "region", value, sizeof(value))) {
        const lorawan_region_t *region = lorawan_find_region(value);
        if(region) {
            lorawan_config.region = region;
        }
    }
    if(lorawan_json_get_int(json, "sub_band", &number)) {
        lorawan_config.sub_band = (uint8_t)number;
    }
    if(lorawan_json_get_string(json, "join_eui", value, sizeof(value))) {
        (void)lorawan_parse_eui(value, &lorawan_config.join_eui);
    }
    if(lorawan_json_get_string(json, "dev_eui", value, sizeof(value))) {
        lorawan_config.has_dev_eui =
            lorawan_parse_eui(value, &lorawan_config.dev_eui);
    }
    if(lorawan_json_get_string(json, "app_key", value, sizeof(value))) {
        lorawan_config.has_app_key =
            lorawan_parse_hex_bytes(value, lorawan_config.app_key, 16);
    }
    if(lorawan_json_get_string(json, "nwk_key", value, sizeof(value))) {
        lorawan_config.has_nwk_key =
            lorawan_parse_hex_bytes(value, lorawan_config.nwk_key, 16);
    }
    if(lorawan_json_get_int(json, "uplink_interval_s", &number) ||
       lorawan_json_get_int(json, "interval_s", &number)) {
        lorawan_config.interval_s = number < 60 ? 60U : (uint32_t)number;
    }
    if(lorawan_json_get_int(json, "fport", &number) && number >= 1 &&
       number <= 223) {
        lorawan_config.fport = (uint8_t)number;
    }
    (void)lorawan_json_get_bool(json, "confirmed", &lorawan_config.confirmed);
    (void)lorawan_json_get_bool(json, "adr", &lorawan_config.adr);
    if(lorawan_json_get_int(json, "datarate", &number) && number >= 0 &&
       number <= 15) {
        lorawan_config.datarate = (uint8_t)number;
    }
    (void)lorawan_json_get_bool(json, "duty_cycle",
                                &lorawan_config.duty_cycle);
    if(lorawan_json_get_int(json, "duty_cycle_ms_per_hour", &number) &&
       number >= 0) {
        lorawan_config.duty_cycle_ms_per_hour = (uint32_t)number;
    }
    (void)lorawan_json_get_bool(json, "dwell_time",
                                &lorawan_config.dwell_time);
    if(lorawan_json_get_int(json, "dwell_time_ms", &number) && number >= 0) {
        lorawan_config.dwell_time_ms = (uint32_t)number;
    }
    (void)lorawan_json_get_bool(json, "simulate_temperature",
                                &lorawan_config.simulate_temperature);
    if(lorawan_json_get_int(json, "tx_power", &number) && number >= -9 &&
       number <= 22) {
        lorawan_config.has_tx_power = 1;
        lorawan_config.tx_power = (int8_t)number;
    }

    lorawan_config_loaded = 1;
    lorawan_config_valid = lorawan_validate_config(0);
    lorawan_config_dirty = 0;
    lorawan_set_selected_profile(path);
    return lorawan_config_valid;
}

static int lorawan_load_conf_path(const char *path, int create_template)
{
    FILE *fp;
    char line[224];

    lorawan_config_defaults();
    lorawan_config_loaded = 0;
    lorawan_config_valid = 0;
    lorawan_config_dirty = 0;

    fp = fopen(path ? path : LORAWAN_CONFIG_PATH, "r");
    if(!fp) {
        if(create_template && lorawan_write_template()) {
            lorawan_set_status("Template created: %s", LORAWAN_CONFIG_PATH);
        } else {
            lorawan_set_status("Config create failed");
        }
        return 0;
    }

    while(fgets(line, sizeof(line), fp)) {
        char *eq;
        char *key;
        char *value;
        char *comment;

        comment = strchr(line, '#');
        if(comment) {
            *comment = '\0';
        }
        ui_trim_text(line);
        if(!line[0]) {
            continue;
        }
        eq = strchr(line, '=');
        if(!eq) {
            continue;
        }
        *eq = '\0';
        key = line;
        value = eq + 1;
        ui_trim_text(key);
        ui_trim_text(value);

        if(!strcasecmp(key, "region")) {
            const lorawan_region_t *region = lorawan_find_region(value);
            if(region) {
                lorawan_config.region = region;
            }
        } else if(!strcasecmp(key, "sub_band")) {
            lorawan_config.sub_band = (uint8_t)atoi(value);
        } else if(!strcasecmp(key, "join_eui") || !strcasecmp(key, "app_eui")) {
            (void)lorawan_parse_eui(value, &lorawan_config.join_eui);
        } else if(!strcasecmp(key, "dev_eui")) {
            lorawan_config.has_dev_eui =
                lorawan_parse_eui(value, &lorawan_config.dev_eui);
        } else if(!strcasecmp(key, "app_key")) {
            lorawan_config.has_app_key =
                lorawan_parse_hex_bytes(value, lorawan_config.app_key, 16);
        } else if(!strcasecmp(key, "nwk_key") ||
                  !strcasecmp(key, "network_key")) {
            lorawan_config.has_nwk_key =
                lorawan_parse_hex_bytes(value, lorawan_config.nwk_key, 16);
        } else if(!strcasecmp(key, "uplink_interval_s") ||
                  !strcasecmp(key, "interval_s")) {
            lorawan_config.interval_s = (uint32_t)atoi(value);
            if(lorawan_config.interval_s < 60U) {
                lorawan_config.interval_s = 60U;
            }
        } else if(!strcasecmp(key, "fport") || !strcasecmp(key, "port")) {
            int port = atoi(value);
            if(port >= 1 && port <= 223) {
                lorawan_config.fport = (uint8_t)port;
            }
        } else if(!strcasecmp(key, "confirmed")) {
            lorawan_config.confirmed =
                lorawan_parse_bool(value, lorawan_config.confirmed);
        } else if(!strcasecmp(key, "adr")) {
            lorawan_config.adr = lorawan_parse_bool(value, lorawan_config.adr);
        } else if(!strcasecmp(key, "datarate")) {
            int dr = atoi(value);
            if(dr >= 0 && dr <= 15) {
                lorawan_config.datarate = (uint8_t)dr;
            }
        } else if(!strcasecmp(key, "duty_cycle")) {
            lorawan_config.duty_cycle =
                lorawan_parse_bool(value, lorawan_config.duty_cycle);
        } else if(!strcasecmp(key, "duty_cycle_ms_per_hour")) {
            lorawan_config.duty_cycle_ms_per_hour = (uint32_t)atoi(value);
        } else if(!strcasecmp(key, "dwell_time")) {
            lorawan_config.dwell_time =
                lorawan_parse_bool(value, lorawan_config.dwell_time);
        } else if(!strcasecmp(key, "dwell_time_ms")) {
            lorawan_config.dwell_time_ms = (uint32_t)atoi(value);
        } else if(!strcasecmp(key, "simulate_temperature") ||
                  !strcasecmp(key, "simulate_temp")) {
            lorawan_config.simulate_temperature =
                lorawan_parse_bool(value, lorawan_config.simulate_temperature);
        } else if(!strcasecmp(key, "tx_power")) {
            int power = atoi(value);
            if(power >= -9 && power <= 22) {
                lorawan_config.has_tx_power = 1;
                lorawan_config.tx_power = (int8_t)power;
            }
        }
    }
    fclose(fp);

    lorawan_config_loaded = 1;
    lorawan_config_valid = lorawan_validate_config(0);
    lorawan_config_dirty = 0;
    lorawan_set_selected_profile(path ? path : LORAWAN_CONFIG_PATH);
    return lorawan_config_valid;
}

static int lorawan_load_config_path(const char *path, int create_template)
{
    if(path && lorawan_has_suffix(path, LORAWAN_PROFILE_EXT)) {
        return lorawan_load_json_path(path);
    }
    return lorawan_load_conf_path(path, create_template);
}

static int lorawan_save_config(void)
{
    const char *path = lorawan_selected_path[0] ? lorawan_selected_path :
                       LORAWAN_CONFIG_PATH;

    if(lorawan_has_suffix(path, LORAWAN_PROFILE_EXT)) {
        return lorawan_save_config_json(path);
    }
    return lorawan_save_config_conf(path);
}

static int lorawan_load_config(void)
{
    return lorawan_load_config_path(LORAWAN_CONFIG_PATH, 1);
}

static int lorawan_autoload_config(void)
{
    char path[LORAWAN_PATH_MAX];

    if(lorawan_selected_path[0] && lorawan_file_exists(lorawan_selected_path)) {
        return lorawan_load_config_path(lorawan_selected_path, 0);
    }
    lorawan_build_profile_path("lora_wan_0.json", path, sizeof(path));
    if(lorawan_file_exists(path)) {
        return lorawan_load_config_path(path, 0);
    }
    if(lorawan_file_exists(LORAWAN_CONFIG_PATH)) {
        return lorawan_load_config_path(LORAWAN_CONFIG_PATH, 0);
    }
    return lorawan_load_config();
}

static void lorawan_read_binary(const char *path, uint8_t *data, size_t len)
{
    FILE *fp = fopen(path, "rb");
    size_t got;

    if(!fp) {
        return;
    }
    got = fread(data, 1, len, fp);
    (void)got;
    fclose(fp);
}

static void lorawan_write_binary(const char *path, const uint8_t *data,
                                 size_t len)
{
    FILE *fp;

    mkdir(LORAWAN_CONFIG_DIR, 0755);
    fp = fopen(path, "wb");
    if(!fp) {
        return;
    }
    (void)fwrite(data, 1, len, fp);
    fclose(fp);
}

static const char *lorawan_state_name(int16_t state)
{
    switch(state) {
    case RADIOLIB_ERR_NONE:
        return "OK";
    case RADIOLIB_ERR_CHIP_NOT_FOUND:
        return "CHIP_NOT_FOUND";
    case RADIOLIB_ERR_NETWORK_NOT_JOINED:
        return "NOT_JOINED";
    case RADIOLIB_ERR_NO_JOIN_ACCEPT:
        return "NO_JOIN_ACCEPT";
    case RADIOLIB_ERR_UPLINK_UNAVAILABLE:
        return "UPLINK_UNAVAILABLE";
    case RADIOLIB_LORAWAN_SESSION_RESTORED:
        return "SESSION_RESTORED";
    case RADIOLIB_LORAWAN_NEW_SESSION:
        return "NEW_SESSION";
    default:
        return lora_error_name(state);
    }
}

static int lorawan_prepare_radio(void)
{
    lora_profile_t probe;

    if(!lorawan_config.region) {
        return -1;
    }
    if(lora_hw_prepare() != 0) {
        return -1;
    }

    memset(&probe, 0, sizeof(probe));
    snprintf(probe.name, sizeof(probe.name), "LoRaWAN %s",
             lorawan_config.region->name);
    probe.freq = lorawan_config.region->rf_freq_mhz;
    probe.bandwidth = 125.0f;
    probe.power = lorawan_config.has_tx_power ? lorawan_config.tx_power : 14;
    probe.sf = 7;
    probe.cr = 5;
    probe.sync_word = 0x34;
    probe.preamble = 8;
    probe.interval_ms = 1000;
    probe.counter_payload = 1;

    if(lora_radio) {
        int16_t state = lora_begin_active_chip(&probe);
        if(state == RADIOLIB_ERR_NONE) {
            return 0;
        }
        lora_log("LoRaWAN active chip re-init failed: %d %s",
                 state, lora_error_name(state));
    }
    return lora_detect_radio(&probe);
}

static void lorawan_update_region_buttons(void)
{
    for(size_t i = 0; i < LORAWAN_REGION_COUNT; i++) {
        int selected = lorawan_config.region == &lorawan_regions[i];

        if(lorawan_config.region &&
           strcmp(lorawan_config.region->name, lorawan_regions[i].name) == 0) {
            selected = 1;
        }
        lora_style_button_selected(lorawan_region_buttons[i], selected,
                                   0x3DA5FF);
    }
}

static void lorawan_update_labels(void)
{
    char text[128];

    if(lorawan_radio_label && lv_obj_is_valid(lorawan_radio_label)) {
        lv_label_set_text(lorawan_radio_label, lora_active_chip_name());
    }
    if(lorawan_config_label && lv_obj_is_valid(lorawan_config_label)) {
        lv_label_set_text(lorawan_config_label,
                          lorawan_config_dirty ? "Unsaved changes" :
                          (lorawan_config_valid ? "Saved config" :
                           "Tap fields to configure"));
    }
    if(lorawan_profile_label && lv_obj_is_valid(lorawan_profile_label)) {
        lv_label_set_text(lorawan_profile_label,
                          lorawan_selected_file[0] ? lorawan_selected_file :
                          "No profile selected");
    }
    if(lorawan_region_label && lv_obj_is_valid(lorawan_region_label)) {
        snprintf(text, sizeof(text), "%s SB%u DR%u %lus",
                 lorawan_config.region ? lorawan_config.region->name : "--",
                 lorawan_config.sub_band, lorawan_config.datarate,
                 (unsigned long)lorawan_config.interval_s);
        lv_label_set_text(lorawan_region_label, text);
    }
    if(lorawan_join_label && lv_obj_is_valid(lorawan_join_label)) {
        lv_label_set_text(lorawan_join_label,
                          lorawan_joined ? "Joined" :
                          (lorawan_config_valid ? "Ready" : "Config needed"));
    }
    if(lorawan_devaddr_label && lv_obj_is_valid(lorawan_devaddr_label)) {
        if(lorawan_joined && lorawan_node) {
            snprintf(text, sizeof(text), "0x%08lX",
                     (unsigned long)lorawan_node->getDevAddr());
        } else {
            snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(lorawan_devaddr_label, text);
    }
    if(lorawan_fcnt_label && lv_obj_is_valid(lorawan_fcnt_label)) {
        if(lorawan_joined && lorawan_node) {
            snprintf(text, sizeof(text), "%lu",
                     (unsigned long)lorawan_node->getFCntUp());
        } else {
            snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(lorawan_fcnt_label, text);
    }
    if(lorawan_join_eui_value_label &&
       lv_obj_is_valid(lorawan_join_eui_value_label)) {
        lorawan_format_eui(lorawan_config.join_eui, text, sizeof(text));
        lv_label_set_text(lorawan_join_eui_value_label, text);
    }
    if(lorawan_region_value_label &&
       lv_obj_is_valid(lorawan_region_value_label)) {
        lv_label_set_text(lorawan_region_value_label,
                          lorawan_config.region ?
                          lorawan_config.region->name : "--");
    }
    if(lorawan_dev_eui_value_label &&
       lv_obj_is_valid(lorawan_dev_eui_value_label)) {
        lorawan_format_eui(lorawan_config.dev_eui, text, sizeof(text));
        lv_label_set_text(lorawan_dev_eui_value_label,
                          lorawan_config.has_dev_eui ? text : "--");
    }
    if(lorawan_app_key_value_label &&
       lv_obj_is_valid(lorawan_app_key_value_label)) {
        lorawan_format_key_short(lorawan_config.app_key,
                                 lorawan_config.has_app_key, text,
                                 sizeof(text));
        lv_label_set_text(lorawan_app_key_value_label, text);
    }
    if(lorawan_nwk_key_value_label &&
       lv_obj_is_valid(lorawan_nwk_key_value_label)) {
        lorawan_format_key_short(lorawan_config.nwk_key,
                                 lorawan_config.has_nwk_key, text,
                                 sizeof(text));
        lv_label_set_text(lorawan_nwk_key_value_label, text);
    }
    if(lorawan_subband_value_label &&
       lv_obj_is_valid(lorawan_subband_value_label)) {
        snprintf(text, sizeof(text), "%u", lorawan_config.sub_band);
        lv_label_set_text(lorawan_subband_value_label, text);
    }
    if(lorawan_interval_value_label &&
       lv_obj_is_valid(lorawan_interval_value_label)) {
        snprintf(text, sizeof(text), "%lus",
                 (unsigned long)lorawan_config.interval_s);
        lv_label_set_text(lorawan_interval_value_label, text);
    }
    if(lorawan_fport_value_label && lv_obj_is_valid(lorawan_fport_value_label)) {
        snprintf(text, sizeof(text), "%u", lorawan_config.fport);
        lv_label_set_text(lorawan_fport_value_label, text);
    }
    if(lorawan_confirmed_switch && lv_obj_is_valid(lorawan_confirmed_switch)) {
        if(lorawan_config.confirmed) {
            lv_obj_add_state(lorawan_confirmed_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(lorawan_confirmed_switch, LV_STATE_CHECKED);
        }
    }
    if(lorawan_adr_switch && lv_obj_is_valid(lorawan_adr_switch)) {
        if(lorawan_config.adr) {
            lv_obj_add_state(lorawan_adr_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(lorawan_adr_switch, LV_STATE_CHECKED);
        }
    }
    if(lorawan_sim_temp_switch && lv_obj_is_valid(lorawan_sim_temp_switch)) {
        if(lorawan_config.simulate_temperature) {
            lv_obj_add_state(lorawan_sim_temp_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(lorawan_sim_temp_switch, LV_STATE_CHECKED);
        }
    }
    lorawan_update_region_buttons();
}

static lv_obj_t *lorawan_info(lv_obj_t *parent, int y, const char *name,
                              const char *value, lv_obj_t **out)
{
    lv_obj_t *label;
    int row_w = lora_panel_content_width(parent);
    int value_w = row_w - 168;
    lv_obj_t *row = ui_panel(parent, 0, y, row_w, 58);

    if(value_w < 150) {
        value_w = 150;
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2028), 0);

    label = ui_label(row, name, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

    label = ui_label(row, value, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(label, value_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -12, 0);
    if(out) {
        *out = label;
    }
    return row;
}

typedef enum {
    LORAWAN_EDIT_JOIN_EUI = 0,
    LORAWAN_EDIT_DEV_EUI,
    LORAWAN_EDIT_APP_KEY,
    LORAWAN_EDIT_NWK_KEY,
    LORAWAN_EDIT_SUB_BAND,
    LORAWAN_EDIT_INTERVAL,
    LORAWAN_EDIT_FPORT,
} lorawan_edit_field_t;

static void lorawan_field_initial(lorawan_edit_field_t field, char *out,
                                  size_t out_len)
{
    if(!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    switch(field) {
    case LORAWAN_EDIT_JOIN_EUI:
        lorawan_format_eui(lorawan_config.join_eui, out, out_len);
        break;
    case LORAWAN_EDIT_DEV_EUI:
        lorawan_format_eui(lorawan_config.dev_eui, out, out_len);
        break;
    case LORAWAN_EDIT_APP_KEY:
        lorawan_format_key(lorawan_config.app_key, out, out_len);
        break;
    case LORAWAN_EDIT_NWK_KEY:
        lorawan_format_key(lorawan_config.nwk_key, out, out_len);
        break;
    case LORAWAN_EDIT_SUB_BAND:
        snprintf(out, out_len, "%u", lorawan_config.sub_band);
        break;
    case LORAWAN_EDIT_INTERVAL:
        snprintf(out, out_len, "%lu",
                 (unsigned long)lorawan_config.interval_s);
        break;
    case LORAWAN_EDIT_FPORT:
        snprintf(out, out_len, "%u", lorawan_config.fport);
        break;
    default:
        break;
    }
}

static const char *lorawan_field_title(lorawan_edit_field_t field)
{
    switch(field) {
    case LORAWAN_EDIT_JOIN_EUI:
        return "JoinEUI";
    case LORAWAN_EDIT_DEV_EUI:
        return "DevEUI";
    case LORAWAN_EDIT_APP_KEY:
        return "AppKey";
    case LORAWAN_EDIT_NWK_KEY:
        return "NwkKey";
    case LORAWAN_EDIT_SUB_BAND:
        return "Sub-band";
    case LORAWAN_EDIT_INTERVAL:
        return "Uplink interval";
    case LORAWAN_EDIT_FPORT:
        return "FPort";
    default:
        return "LoRaWAN field";
    }
}

static const char *lorawan_field_hint(lorawan_edit_field_t field)
{
    switch(field) {
    case LORAWAN_EDIT_JOIN_EUI:
    case LORAWAN_EDIT_DEV_EUI:
        return "16 hex digits";
    case LORAWAN_EDIT_APP_KEY:
    case LORAWAN_EDIT_NWK_KEY:
        return "32 hex digits";
    case LORAWAN_EDIT_SUB_BAND:
        return "0 for default";
    case LORAWAN_EDIT_INTERVAL:
        return "seconds, minimum 60";
    case LORAWAN_EDIT_FPORT:
        return "1-223";
    default:
        return "";
    }
}

static void lorawan_field_submit_cb(const char *text, void *user_data)
{
    lorawan_edit_field_t field =
        (lorawan_edit_field_t)(intptr_t)user_data;
    char value[96];

    snprintf(value, sizeof(value), "%s", text ? text : "");
    ui_trim_text(value);
    if(!value[0]) {
        lorawan_set_status("Empty value ignored");
        return;
    }

    switch(field) {
    case LORAWAN_EDIT_JOIN_EUI:
        if(!lorawan_parse_eui(value, &lorawan_config.join_eui)) {
            lorawan_set_status("JoinEUI must be 16 hex digits");
            return;
        }
        break;
    case LORAWAN_EDIT_DEV_EUI:
        lorawan_config.has_dev_eui =
            lorawan_parse_eui(value, &lorawan_config.dev_eui);
        if(!lorawan_config.has_dev_eui) {
            lorawan_set_status("DevEUI must be 16 hex digits");
            return;
        }
        break;
    case LORAWAN_EDIT_APP_KEY:
        lorawan_config.has_app_key =
            lorawan_parse_hex_bytes(value, lorawan_config.app_key, 16);
        if(!lorawan_config.has_app_key) {
            lorawan_set_status("AppKey must be 32 hex digits");
            return;
        }
        break;
    case LORAWAN_EDIT_NWK_KEY:
        lorawan_config.has_nwk_key =
            lorawan_parse_hex_bytes(value, lorawan_config.nwk_key, 16);
        if(!lorawan_config.has_nwk_key) {
            lorawan_set_status("NwkKey must be 32 hex digits");
            return;
        }
        break;
    case LORAWAN_EDIT_SUB_BAND: {
        int sub_band = atoi(value);
        if(sub_band < 0 || sub_band > 15) {
            lorawan_set_status("Sub-band invalid");
            return;
        }
        lorawan_config.sub_band = (uint8_t)sub_band;
        break;
    }
    case LORAWAN_EDIT_INTERVAL: {
        int interval = atoi(value);
        if(interval < 60) {
            interval = 60;
        }
        lorawan_config.interval_s = (uint32_t)interval;
        break;
    }
    case LORAWAN_EDIT_FPORT: {
        int fport = atoi(value);
        if(fport < 1 || fport > 223) {
            lorawan_set_status("FPort invalid");
            return;
        }
        lorawan_config.fport = (uint8_t)fport;
        break;
    }
    default:
        break;
    }

    lorawan_reset_node();
    lorawan_config_valid = lorawan_validate_config(0);
    lorawan_config_dirty = 1;
    lorawan_set_status("Changed, save profile");
    lorawan_update_labels();
}

static void lorawan_field_event_cb(lv_event_t *event)
{
    lorawan_edit_field_t field =
        (lorawan_edit_field_t)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t config;
    char value[96];

    lorawan_field_initial(field, value, sizeof(value));
    memset(&config, 0, sizeof(config));
    config.title = lorawan_field_title(field);
    config.placeholder = lorawan_field_hint(field);
    config.initial_text = value;
    config.password_mode = (field == LORAWAN_EDIT_APP_KEY ||
                            field == LORAWAN_EDIT_NWK_KEY);
    config.max_length = (field == LORAWAN_EDIT_APP_KEY ||
                         field == LORAWAN_EDIT_NWK_KEY) ? 64U : 32U;
    config.submit_cb = lorawan_field_submit_cb;
    config.user_data = (void *)(intptr_t)field;
    config.submit_text = "Apply";
    ui_input_dialog_open(&config);
}

static lv_obj_t *lorawan_config_row(lv_obj_t *parent, int y,
                                    const char *name, lv_obj_t **value_out,
                                    lorawan_edit_field_t field)
{
    lv_obj_t *row;
    lv_obj_t *label;
    int row_w = lora_panel_content_width(parent);
    int value_w = row_w - 176;

    if(value_w < 150) {
        value_w = 150;
    }
    row = lora_button(parent, 0, y, row_w, 52, "", 0xF2F5F8);
    label = ui_label(row, name, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

    label = ui_label(row, "--", &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_width(label, value_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -12, 0);
    if(value_out) {
        *value_out = label;
    }
    lv_obj_add_event_cb(row, lorawan_field_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)field);
    return row;
}

static void lorawan_region_event_cb(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);

    if(index >= LORAWAN_REGION_COUNT) {
        return;
    }
    lorawan_config.region = &lorawan_regions[index];
    lorawan_reset_node();
    lorawan_config_dirty = 1;
    lorawan_set_status("Region changed: %s", lorawan_regions[index].name);
    lorawan_update_labels();
}

static int lorawan_region_grid(lv_obj_t *parent, int y, int landscape)
{
    lv_obj_t *title;
    int row_w = lora_panel_content_width(parent);
    int gap = 10;
    int cols = landscape ? 4 : 3;
    int button_w;
    int button_h = landscape ? 38 : 42;
    int start_y = y + 34;

    if(row_w < 360) {
        cols = 2;
    }
    button_w = (row_w - gap * (cols - 1)) / cols;
    if(button_w < 80) {
        button_w = 80;
    }

    title = ui_label(parent, "Region", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, y);

    for(size_t i = 0; i < LORAWAN_REGION_COUNT; i++) {
        int col = (int)i % cols;
        int row = (int)i / cols;
        int x = col * (button_w + gap);
        int by = start_y + row * (button_h + gap);

        lorawan_region_buttons[i] =
            lora_button(parent, x, by, button_w, button_h,
                        lorawan_regions[i].name, 0xF2F5F8);
        lv_obj_add_event_cb(lorawan_region_buttons[i],
                            lorawan_region_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
    return start_y +
           (((int)LORAWAN_REGION_COUNT + cols - 1) / cols) *
           (button_h + gap);
}

static void lorawan_switch_event_cb(lv_event_t *event)
{
    int which = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(event);
    int checked = sw && lv_obj_has_state(sw, LV_STATE_CHECKED);

    if(which == 0) {
        lorawan_config.confirmed = checked;
    } else if(which == 1) {
        lorawan_config.adr = checked;
        if(lorawan_node) {
            lorawan_node->setADR(checked != 0);
        }
    } else {
        lorawan_config.simulate_temperature = checked;
    }
    lorawan_config_dirty = 1;
    lorawan_set_status("Changed, save profile");
    lorawan_update_labels();
}

static lv_obj_t *lorawan_switch_row(lv_obj_t *parent, int y,
                                    const char *name, lv_obj_t **sw_out,
                                    int which)
{
    int row_w = lora_panel_content_width(parent);
    lv_obj_t *row = ui_panel(parent, 0, y, row_w, 52);
    lv_obj_t *label;
    lv_obj_t *sw;

    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2028), 0);
    label = ui_label(row, name, &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_width(label, row_w - 100);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    sw = lv_switch_create(row);
    lv_obj_set_size(sw, 66, 34);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(sw, lorawan_switch_event_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)which);
    if(sw_out) {
        *sw_out = sw;
    }
    return row;
}

static void lorawan_reset_node(void)
{
    if(lorawan_node) {
        delete lorawan_node;
        lorawan_node = NULL;
    }
    lorawan_joined = 0;
}

static void lorawan_apply_node_settings(void)
{
    if(!lorawan_node) {
        return;
    }
    lorawan_node->setADR(lorawan_config.adr != 0);
    lorawan_node->setDatarate(lorawan_config.datarate);
    lorawan_node->setDutyCycle(lorawan_config.duty_cycle != 0,
                               lorawan_config.duty_cycle_ms_per_hour);
    lorawan_node->setDwellTime(lorawan_config.dwell_time != 0,
                               lorawan_config.dwell_time_ms);
    if(lorawan_config.has_tx_power) {
        lorawan_node->setTxPower(lorawan_config.tx_power);
    }
}

static void lorawan_refresh_view(lorawan_view_t view)
{
    lorawan_view = view;
    app_refresh_current_page();
}

static void lorawan_save_name_submit_cb(const char *text, void *user_data)
{
    char file[LORAWAN_FILE_MAX];
    char path[LORAWAN_PATH_MAX];
    char raw[LORAWAN_FILE_MAX];

    (void)user_data;
    snprintf(raw, sizeof(raw), "%s", text ? text : "");
    ui_trim_text(raw);
    if(!raw[0]) {
        lorawan_default_profile_name(file, sizeof(file));
    } else {
        lorawan_sanitize_profile_file(raw, file, sizeof(file));
    }
    lorawan_build_profile_path(file, path, sizeof(path));
    lorawan_reset_node();
    if(lorawan_save_config_json(path)) {
        lorawan_refresh_view(LORAWAN_VIEW_HOME);
    } else {
        lorawan_update_labels();
    }
}

static void lorawan_save_event(lv_event_t *event)
{
    ui_input_dialog_config_t config;
    char file[LORAWAN_FILE_MAX];

    (void)event;
    if(!lorawan_validate_config(1)) {
        lorawan_config_valid = 0;
        lorawan_update_labels();
        return;
    }
    if(lorawan_selected_file[0] &&
       lorawan_has_suffix(lorawan_selected_file, LORAWAN_PROFILE_EXT)) {
        snprintf(file, sizeof(file), "%s", lorawan_selected_file);
    } else {
        lorawan_default_profile_name(file, sizeof(file));
    }

    memset(&config, 0, sizeof(config));
    config.title = "Config file name";
    config.placeholder = "lora_wan_0.json";
    config.initial_text = file;
    config.max_length = LORAWAN_FILE_MAX - 1;
    config.submit_cb = lorawan_save_name_submit_cb;
    config.submit_text = "Save";
    ui_input_dialog_open(&config);
}

static void lorawan_load_view_event(lv_event_t *event)
{
    (void)event;
    (void)lorawan_scan_profiles();
    lorawan_refresh_view(LORAWAN_VIEW_LOAD);
}

static void lorawan_edit_view_event(lv_event_t *event)
{
    (void)event;
    if(!lorawan_config_loaded && !lorawan_autoload_config()) {
        lorawan_set_status("Load or create a profile first");
        lorawan_update_labels();
        return;
    }
    lorawan_refresh_view(LORAWAN_VIEW_EDIT);
}

static void lorawan_new_event(lv_event_t *event)
{
    (void)event;
    lorawan_reset_node();
    lorawan_config_defaults();
    lorawan_set_selected_profile(NULL);
    lorawan_config_loaded = 1;
    lorawan_config_valid = 0;
    lorawan_config_dirty = 1;
    lorawan_set_status("New LoRaWAN profile");
    lorawan_refresh_view(LORAWAN_VIEW_EDIT);
}

static void lorawan_back_home_event(lv_event_t *event)
{
    (void)event;
    lorawan_refresh_view(LORAWAN_VIEW_HOME);
}

static void lorawan_profile_load_event(lv_event_t *event)
{
    const char *file = (const char *)lv_event_get_user_data(event);
    char path[LORAWAN_PATH_MAX];

    if(!file || !file[0]) {
        return;
    }
    lorawan_build_profile_path(file, path, sizeof(path));
    lorawan_reset_node();
    if(lorawan_load_config_path(path, 0)) {
        lorawan_set_status("Profile loaded: %s", file);
        lorawan_refresh_view(LORAWAN_VIEW_HOME);
    } else {
        lorawan_set_status("Profile invalid: %s", file);
        lorawan_update_labels();
    }
}

static void lorawan_profile_edit_event(lv_event_t *event)
{
    const char *file = (const char *)lv_event_get_user_data(event);
    char path[LORAWAN_PATH_MAX];

    if(!file || !file[0]) {
        return;
    }
    lorawan_build_profile_path(file, path, sizeof(path));
    lorawan_reset_node();
    if(lorawan_load_config_path(path, 0)) {
        lorawan_set_status("Profile loaded: %s", file);
        lorawan_refresh_view(LORAWAN_VIEW_EDIT);
    } else {
        lorawan_set_status("Profile invalid: %s", file);
        lorawan_update_labels();
    }
}

static void lorawan_join_event(lv_event_t *event)
{
    int16_t state;
    uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {0};
    uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {0};

    (void)event;
    if(!lorawan_config_loaded) {
        (void)lorawan_autoload_config();
    }
    if(lorawan_config_dirty) {
        lorawan_set_status("Save profile first");
        lorawan_update_labels();
        return;
    }
    lorawan_config_valid = lorawan_validate_config(1);
    if(!lorawan_config_valid) {
        lorawan_update_labels();
        return;
    }
    lorawan_set_status("Preparing radio...");
    lv_refr_now(NULL);
    if(lorawan_prepare_radio() != 0 || !lora_radio) {
        lorawan_set_status("Radio not detected");
        lorawan_update_labels();
        return;
    }

    lorawan_reset_node();
    lorawan_node = new LoRaWANNode(lora_radio, lorawan_config.region->band,
                                   lorawan_config.sub_band);
    if(!lorawan_node) {
        lorawan_set_status("Node allocation failed");
        return;
    }

    state = lorawan_node->beginOTAA(lorawan_config.join_eui,
                                    lorawan_config.dev_eui,
                                    lorawan_config.nwk_key,
                                    lorawan_config.app_key);
    if(state != RADIOLIB_ERR_NONE) {
        lorawan_set_status("OTAA init failed: %s (%d)",
                           lorawan_state_name(state), state);
        lorawan_update_labels();
        return;
    }

    lorawan_read_binary(LORAWAN_NONCES_PATH, nonces, sizeof(nonces));
    (void)lorawan_node->setBufferNonces(nonces);
    lorawan_read_binary(LORAWAN_SESSION_PATH, session, sizeof(session));
    (void)lorawan_node->setBufferSession(session);

    lorawan_set_status("Joining...");
    lv_refr_now(NULL);
    state = lorawan_node->activateOTAA();
    lorawan_write_binary(LORAWAN_NONCES_PATH, lorawan_node->getBufferNonces(),
                         RADIOLIB_LORAWAN_NONCES_BUF_SIZE);

    if(state != RADIOLIB_LORAWAN_SESSION_RESTORED &&
       state != RADIOLIB_LORAWAN_NEW_SESSION) {
        lorawan_joined = 0;
        lorawan_set_status("Join failed: %s (%d)",
                           lorawan_state_name(state), state);
        lorawan_update_labels();
        return;
    }

    lorawan_joined = 1;
    lorawan_apply_node_settings();
    lorawan_write_binary(LORAWAN_SESSION_PATH, lorawan_node->getBufferSession(),
                         RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    lorawan_set_status("Joined: %s", lorawan_state_name(state));
    lorawan_update_labels();
}

static void lorawan_send_event(lv_event_t *event)
{
    uint8_t payload[16];
    size_t payload_len = 12;
    uint8_t downlink[64];
    size_t downlink_len = 0;
    int16_t state;
    uint32_t now_s = (uint32_t)(ui_monotonic_us() / 1000000ULL);
    int temp_deci_c = 0;

    (void)event;
    if(!lorawan_joined || !lorawan_node) {
        lorawan_set_status("Join first");
        return;
    }

    memset(payload, 0, sizeof(payload));
    if(lorawan_config.simulate_temperature) {
        temp_deci_c = 220 + (int)((now_s * 7U) % 120U);
        payload[0] = LORAWAN_PAYLOAD_VERSION;
        payload[1] = 'T';
        payload[2] = (uint8_t)((temp_deci_c >> 8) & 0xFF);
        payload[3] = (uint8_t)(temp_deci_c & 0xFF);
        payload[4] = (uint8_t)((now_s >> 24) & 0xFF);
        payload[5] = (uint8_t)((now_s >> 16) & 0xFF);
        payload[6] = (uint8_t)((now_s >> 8) & 0xFF);
        payload[7] = (uint8_t)(now_s & 0xFF);
        payload_len = 8;
    } else {
        payload[0] = LORAWAN_PAYLOAD_VERSION;
        payload[1] = (uint8_t)lora_chip_type;
        payload[2] = (uint8_t)((now_s >> 24) & 0xFF);
        payload[3] = (uint8_t)((now_s >> 16) & 0xFF);
        payload[4] = (uint8_t)((now_s >> 8) & 0xFF);
        payload[5] = (uint8_t)(now_s & 0xFF);
        payload[6] = (uint8_t)lorawan_config.datarate;
        payload[7] = lorawan_config.confirmed ? 1U : 0U;
        payload[8] = (uint8_t)lorawan_config.sub_band;
        payload_len = 12;
    }

    lorawan_set_status("Sending...");
    lv_refr_now(NULL);
    state = lorawan_node->sendReceive(payload, payload_len,
                                      lorawan_config.fport, downlink,
                                      &downlink_len,
                                      lorawan_config.confirmed != 0);
    lorawan_write_binary(LORAWAN_SESSION_PATH, lorawan_node->getBufferSession(),
                         RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    if(state < RADIOLIB_ERR_NONE) {
        lorawan_set_status("Send failed: %s (%d)",
                           lorawan_state_name(state), state);
    } else {
        char text[96];
        if(lorawan_config.simulate_temperature) {
            snprintf(text, sizeof(text), "Sim temp %.1f C  FCnt=%lu",
                     (double)temp_deci_c / 10.0,
                     (unsigned long)lorawan_node->getFCntUp());
        } else {
            snprintf(text, sizeof(text), "Sent FCnt=%lu",
                     (unsigned long)lorawan_node->getFCntUp());
        }
        if(lorawan_payload_label && lv_obj_is_valid(lorawan_payload_label)) {
            lv_label_set_text(lorawan_payload_label, text);
        }
        if(lorawan_downlink_label && lv_obj_is_valid(lorawan_downlink_label)) {
            snprintf(text, sizeof(text), "%u downlink %u B",
                     (unsigned)state, (unsigned)downlink_len);
            lv_label_set_text(lorawan_downlink_label, text);
        }
        lorawan_set_status("Send OK");
    }
    lorawan_update_labels();
}

static void lorawan_reset_event(lv_event_t *event)
{
    (void)event;
    lorawan_reset_node();
    unlink(LORAWAN_SESSION_PATH);
    unlink(LORAWAN_NONCES_PATH);
    lorawan_set_status("Session reset");
    lorawan_update_labels();
}

static void lorawan_home_view(lv_obj_t *body, int landscape)
{
    lv_obj_t *status;
    lv_obj_t *session;
    lv_obj_t *btn;
    lv_obj_t *profile_row;
    int body_h = ui_body_height(144);
    int content_w = ui_screen_width() - 48;
    int gap = landscape ? 18 : 20;
    int left_w = landscape ? (content_w * 44) / 100 : content_w;
    int right_w = landscape ? content_w - left_w - gap : content_w;
    int panel_h = landscape ? body_h - 48 : 350;
    int session_x = landscape ? 24 + left_w + gap : 24;
    int session_y = landscape ? 24 : 24 + panel_h + 20;
    int session_h = landscape ? panel_h : 520;
    int action_w;
    int inner_w;
    int y;

    if(landscape && left_w < 360) {
        left_w = 360;
        right_w = content_w - left_w - gap;
    }
    if(right_w < 360) {
        right_w = 360;
    }

    status = ui_panel(body, 24, 24, left_w, panel_h);
    lv_obj_set_style_pad_all(status, 18, 0);
    ui_label(status, "Status", &lv_font_montserrat_24, 0xF2F5F8);
    lorawan_info(status, 48, "Radio", "Auto LoRa", &lorawan_radio_label);
    profile_row = lorawan_info(status, 106, "Profile", "No profile selected",
                               &lorawan_profile_label);
    lv_obj_add_flag(profile_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(profile_row, lorawan_edit_view_event,
                        LV_EVENT_CLICKED, NULL);
    lorawan_info(status, 164, "Config", "Tap fields to configure",
                 &lorawan_config_label);
    lorawan_info(status, 222, "Region", "--", &lorawan_region_label);
    lorawan_status_label =
        ui_label(status, "Loading", &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_width(lorawan_status_label, lora_panel_content_width(status));
    lv_label_set_long_mode(lorawan_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(lorawan_status_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    session = ui_panel(body, session_x, session_y, right_w, session_h);
    lv_obj_set_style_pad_all(session, 18, 0);
    ui_make_scrollable(session, 32);
    ui_label(session, "Session", &lv_font_montserrat_24, 0xF2F5F8);
    lorawan_info(session, 48, "Join", "--", &lorawan_join_label);
    lorawan_info(session, 106, "DevAddr", "--", &lorawan_devaddr_label);
    lorawan_info(session, 164, "FCntUp", "--", &lorawan_fcnt_label);

    y = 234;
    ui_label(session, "Control", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(lv_obj_get_child(session,
                                    lv_obj_get_child_count(session) - 1),
                   0, y);
    y += 42;
    inner_w = lora_panel_content_width(session);
    action_w = (inner_w - 14) / 2;
    if(action_w < 120) {
        action_w = 120;
    }
    btn = lora_button(session, 0, y, action_w, 54, "Load", 0x3DA5FF);
    lv_obj_add_event_cb(btn, lorawan_load_view_event, LV_EVENT_CLICKED, NULL);
    btn = lora_button(session, action_w + 14, y, action_w, 54, "New",
                      0x25C281);
    lv_obj_add_event_cb(btn, lorawan_new_event, LV_EVENT_CLICKED, NULL);
    y += 66;
    btn = lora_button(session, 0, y, action_w, 54, "Edit", 0xB982FF);
    lv_obj_add_event_cb(btn, lorawan_edit_view_event, LV_EVENT_CLICKED, NULL);
    btn = lora_button(session, action_w + 14, y, action_w, 54, "Run",
                      0xF5A524);
    lv_obj_add_event_cb(btn, lorawan_join_event, LV_EVENT_CLICKED, NULL);
    y += 70;
    lorawan_switch_row(session, y, "Upload simulated temperature",
                       &lorawan_sim_temp_switch, 2);
    y += 68;
    btn = lora_button(session, 0, y, (inner_w - 14) / 2, 54,
                      "Send uplink", 0x14B8A6);
    lv_obj_add_event_cb(btn, lorawan_send_event, LV_EVENT_CLICKED, NULL);
    btn = lora_button(session, (inner_w + 14) / 2, y, (inner_w - 14) / 2, 54,
                      "Reset session", 0xEF4D5A);
    lv_obj_add_event_cb(btn, lorawan_reset_event, LV_EVENT_CLICKED, NULL);
    y += 70;
    lorawan_payload_label =
        ui_label(session, "Payload idle", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(lorawan_payload_label, (inner_w - 20) / 2);
    lv_label_set_long_mode(lorawan_payload_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lorawan_payload_label, 0, y);
    lorawan_downlink_label =
        ui_label(session, "Downlink none", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(lorawan_downlink_label, (inner_w - 20) / 2);
    lv_label_set_long_mode(lorawan_downlink_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lorawan_downlink_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lorawan_downlink_label, inner_w / 2 + 10, y);
}

static void lorawan_edit_view(lv_obj_t *body, int landscape)
{
    lv_obj_t *config_panel;
    lv_obj_t *btn;
    lv_obj_t *path_label;
    int body_h = ui_body_height(144);
    int content_w = ui_screen_width() - 48;
    int config_h = landscape ? body_h - 48 : 960;
    int inner_w;
    int action_w;
    int y;

    config_panel = ui_panel(body, 24, 24, content_w, config_h);
    lv_obj_set_style_pad_all(config_panel, 18, 0);
    ui_make_scrollable(config_panel, 48);
    ui_label(config_panel, "LoRaWAN profile", &lv_font_montserrat_24,
             0xF2F5F8);
    path_label = ui_label(config_panel,
                          lorawan_selected_file[0] ? lorawan_selected_file :
                          "Unsaved new profile",
                          &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_pos(path_label, 0, 34);
    lv_obj_set_width(path_label, lora_panel_content_width(config_panel));
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);
    inner_w = lora_panel_content_width(config_panel);
    action_w = (inner_w - 18) / 2;
    if(action_w < 130) {
        action_w = 130;
    }
    btn = lora_button(config_panel, 0, 62, action_w, 54, "Back", 0x3A4250);
    lv_obj_add_event_cb(btn, lorawan_back_home_event, LV_EVENT_CLICKED, NULL);
    btn = lora_button(config_panel, action_w + 18, 62, action_w, 54,
                      "Save profile", 0x25C281);
    lv_obj_add_event_cb(btn, lorawan_save_event, LV_EVENT_CLICKED, NULL);

    y = lorawan_region_grid(config_panel, 138, landscape) + 12;
    lorawan_config_row(config_panel, y, "JoinEUI",
                       &lorawan_join_eui_value_label, LORAWAN_EDIT_JOIN_EUI);
    y += 62;
    lorawan_config_row(config_panel, y, "DevEUI",
                       &lorawan_dev_eui_value_label, LORAWAN_EDIT_DEV_EUI);
    y += 62;
    lorawan_config_row(config_panel, y, "AppKey",
                       &lorawan_app_key_value_label, LORAWAN_EDIT_APP_KEY);
    y += 62;
    lorawan_config_row(config_panel, y, "NwkKey",
                       &lorawan_nwk_key_value_label, LORAWAN_EDIT_NWK_KEY);
    y += 62;
    lorawan_config_row(config_panel, y, "Sub-band",
                       &lorawan_subband_value_label, LORAWAN_EDIT_SUB_BAND);
    y += 62;
    lorawan_config_row(config_panel, y, "Interval",
                       &lorawan_interval_value_label, LORAWAN_EDIT_INTERVAL);
    y += 62;
    lorawan_config_row(config_panel, y, "FPort",
                       &lorawan_fport_value_label, LORAWAN_EDIT_FPORT);
    y += 62;
    lorawan_switch_row(config_panel, y, "Confirmed uplink",
                       &lorawan_confirmed_switch, 0);
    y += 62;
    lorawan_switch_row(config_panel, y, "ADR", &lorawan_adr_switch, 1);
    y += 66;
    btn = lora_button(config_panel, 0, y, inner_w, 54, "Save profile",
                      0x25C281);
    lv_obj_add_event_cb(btn, lorawan_save_event, LV_EVENT_CLICKED, NULL);
}

static void lorawan_load_view(lv_obj_t *body, int landscape)
{
    lv_obj_t *panel;
    lv_obj_t *btn;
    lv_obj_t *label;
    int body_h = ui_body_height(144);
    int content_w = ui_screen_width() - 48;
    int panel_h = landscape ? body_h - 48 : 760;
    int inner_w;
    int cols = landscape ? 2 : 1;
    int row_h = 58;
    int gap = 12;
    int row_w;
    int y = 118;

    (void)lorawan_scan_profiles();
    panel = ui_panel(body, 24, 24, content_w, panel_h);
    lv_obj_set_style_pad_all(panel, 18, 0);
    ui_make_scrollable(panel, 48);
    ui_label(panel, "LoRaWAN profiles", &lv_font_montserrat_24, 0xF2F5F8);
    label = ui_label(panel, "Tap to load, long press to edit",
                     &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(label, 0, 34);
    lv_obj_set_width(label, lora_panel_content_width(panel));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    inner_w = lora_panel_content_width(panel);
    btn = lora_button(panel, 0, 70, inner_w, 42, "Back", 0x3A4250);
    lv_obj_add_event_cb(btn, lorawan_back_home_event, LV_EVENT_CLICKED, NULL);

    if(lorawan_profile_count <= 0) {
        label = ui_label(panel, "No LoRaWAN profiles",
                         &lv_font_montserrat_20, 0xF2F5F8);
        lv_obj_set_pos(label, 0, y + 24);
        return;
    }

    if(inner_w < 520) {
        cols = 1;
    }
    row_w = (inner_w - gap * (cols - 1)) / cols;
    for(int i = 0; i < lorawan_profile_count; i++) {
        int col = i % cols;
        int row = i / cols;
        int x = col * (row_w + gap);
        int by = y + row * (row_h + gap);

        btn = lora_button(panel, x, by, row_w, row_h,
                          lorawan_profile_files[i], 0xF2F5F8);
        lv_obj_add_event_cb(btn, lorawan_profile_load_event,
                            LV_EVENT_CLICKED, lorawan_profile_files[i]);
        lv_obj_add_event_cb(btn, lorawan_profile_edit_event,
                            LV_EVENT_LONG_PRESSED, lorawan_profile_files[i]);
    }
}

void ui_lorawan_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    int landscape = ui_is_landscape();

    memset(lorawan_region_buttons, 0, sizeof(lorawan_region_buttons));
    if(!lorawan_config_loaded && !lorawan_config_dirty) {
        (void)lorawan_autoload_config();
    }

    ui_create_header(scr, "LoRaWAN");
    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    switch(lorawan_view) {
    case LORAWAN_VIEW_LOAD:
        lorawan_load_view(body, landscape);
        break;
    case LORAWAN_VIEW_EDIT:
        lorawan_edit_view(body, landscape);
        break;
    case LORAWAN_VIEW_HOME:
    default:
        lorawan_home_view(body, landscape);
        break;
    }
    if(lorawan_status_label && lv_obj_is_valid(lorawan_status_label)) {
        lv_label_set_text(lorawan_status_label,
                          lorawan_config_valid ? "Ready" :
                          "Create or load a profile");
    }
    lorawan_update_labels();
}

void ui_lorawan_cleanup(void)
{
    lorawan_reset_node();
    lora_delete_radio_objects();
    lorawan_status_label = NULL;
    lorawan_radio_label = NULL;
    lorawan_config_label = NULL;
    lorawan_profile_label = NULL;
    lorawan_region_label = NULL;
    lorawan_join_label = NULL;
    lorawan_devaddr_label = NULL;
    lorawan_fcnt_label = NULL;
    lorawan_payload_label = NULL;
    lorawan_downlink_label = NULL;
    lorawan_region_value_label = NULL;
    lorawan_join_eui_value_label = NULL;
    lorawan_dev_eui_value_label = NULL;
    lorawan_app_key_value_label = NULL;
    lorawan_nwk_key_value_label = NULL;
    lorawan_subband_value_label = NULL;
    lorawan_interval_value_label = NULL;
    lorawan_fport_value_label = NULL;
    lorawan_confirmed_switch = NULL;
    lorawan_adr_switch = NULL;
    lorawan_sim_temp_switch = NULL;
}

void ui_lora_cleanup(void)
{
    lora_editor_close();
    lora_close_carrier_confirm();
    if(lora_timer) {
        lv_timer_delete(lora_timer);
        lora_timer = NULL;
    }
    if(lora_startup_timer) {
        lv_timer_delete(lora_startup_timer);
        lora_startup_timer = NULL;
    }

    lora_delete_radio_objects();
    if(lora_hal) {
        delete lora_hal;
        lora_hal = NULL;
    }

    lora_initialized = 0;
    lora_rx_running = 0;
    lora_tx_pending = 0;
    lora_auto_tx = 0;
    lora_session_mode = LORA_SESSION_LISTEN;
    lora_active_op = LORA_OP_IDLE;
    lora_message_count = 0;
    lora_take_radio_events();
    lora_chip_label = NULL;
    lora_status_label = NULL;
    lora_profile_label = NULL;
    lora_tx_label = NULL;
    lora_rx_label = NULL;
    lora_log_label = NULL;
    lora_auto_label = NULL;
    lora_payload_label = NULL;
    lora_tab_factory = NULL;
    lora_tab_radio = NULL;
    lora_tab_log = NULL;
    lora_factory_panel = NULL;
    lora_chat_panel = NULL;
    lora_log_panel = NULL;
    lora_add_button = NULL;
    lora_editor_overlay = NULL;
    lora_carrier_confirm_overlay = NULL;
    lora_editor_delete_button = NULL;
    memset(lora_editor_value_labels, 0, sizeof(lora_editor_value_labels));
    memset(lora_profile_buttons, 0, sizeof(lora_profile_buttons));
    memset(lora_session_buttons, 0, sizeof(lora_session_buttons));
    memset(lora_payload_buttons, 0, sizeof(lora_payload_buttons));
    lora_payload_row_label = NULL;
    lora_message_page = NULL;
    lora_message_cont = NULL;
}

typedef enum {
    LORA_FLRC_MODE_IDLE = 0,
    LORA_FLRC_MODE_TX,
    LORA_FLRC_MODE_RX,
} lora_flrc_mode_t;

static pthread_mutex_t lora_flrc_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *lora_flrc_timer;
static lv_obj_t *lora_flrc_status_label;
static lv_obj_t *lora_flrc_config_label;
static lv_obj_t *lora_flrc_stats_label;
static lv_obj_t *lora_flrc_log_label;
static lv_obj_t *lora_flrc_preview_image;
static lv_obj_t *lora_flrc_preview_placeholder;
static lv_obj_t *lora_flrc_preview_placeholder_label;
static lv_obj_t *lora_flrc_settings_overlay;
static lv_obj_t *lora_flrc_freq_btn[2];
static lv_obj_t *lora_flrc_br_btn[3];
static lv_obj_t *lora_flrc_camera_preset_btn[
    sizeof(lora_flrc_camera_presets) / sizeof(lora_flrc_camera_presets[0])];
static lv_obj_t *lora_flrc_camera_flip_btn[2];
static int lora_flrc_worker_active;
static int lora_flrc_stop_requested;
static lora_flrc_mode_t lora_flrc_mode = LORA_FLRC_MODE_IDLE;
static float lora_flrc_freq_mhz = LORA_FLRC_DEFAULT_FREQ;
static unsigned lora_flrc_bitrate_kbps = LORA_FLRC_DEFAULT_BR;
static unsigned lora_flrc_camera_preset_index = 1;
static unsigned lora_flrc_camera_preview_w = LORA_FLRC_CAMERA_PREVIEW_W_DEFAULT;
static unsigned lora_flrc_camera_preview_h = LORA_FLRC_CAMERA_PREVIEW_H_DEFAULT;
static unsigned lora_flrc_camera_preview_fps = 4;
static unsigned lora_flrc_camera_jpeg_quality = 28;
static int lora_flrc_camera_flip_x;
static int lora_flrc_camera_flip_y;
static double lora_flrc_video_mbps;
static double lora_flrc_video_file_mbps;
static uint64_t lora_flrc_video_packets;
static uint64_t lora_flrc_video_bytes;
static uint64_t lora_flrc_video_errors;
static uint64_t lora_flrc_video_frames;
static uint64_t lora_flrc_video_dropped;
static uint64_t lora_flrc_video_elapsed_ms;
static uint64_t lora_flrc_start_us;
static uint64_t lora_flrc_packets;
static uint64_t lora_flrc_bytes;
static uint64_t lora_flrc_errors;
static uint64_t lora_flrc_last_stats_log_us;
static int16_t lora_flrc_last_state;
static float lora_flrc_last_rssi_avg;
static float lora_flrc_last_rssi_sync;
static char lora_flrc_status[160] = "Ready";
static char lora_flrc_log_text[640] = "Use two LR2021 boards: one TX and one RX.";
static pid_t lora_flrc_video_pid = -1;
static char lora_flrc_video_role[16] = "";
static uint8_t *lora_flrc_preview_pixels;
static lv_image_dsc_t lora_flrc_preview_dsc;
static uint64_t lora_flrc_preview_sig;
static int lora_flrc_preview_panel_w;
static int lora_flrc_preview_panel_h;

static void lora_flrc_timer_cb(lv_timer_t *timer);

static size_t lora_flrc_preview_bytes(void)
{
    return (size_t)lora_flrc_camera_preview_w *
           (size_t)lora_flrc_camera_preview_h * 2U;
}

static void lora_flrc_apply_camera_preset(unsigned index)
{
    const lora_flrc_camera_preset_t *preset;
    unsigned count = (unsigned)(sizeof(lora_flrc_camera_presets) /
                                sizeof(lora_flrc_camera_presets[0]));

    if(index >= count) {
        index = 1U;
    }
    preset = &lora_flrc_camera_presets[index];
    lora_flrc_camera_preset_index = index;
    lora_flrc_camera_preview_w = preset->width;
    lora_flrc_camera_preview_h = preset->height;
    lora_flrc_camera_preview_fps = preset->fps;
    lora_flrc_camera_jpeg_quality = preset->jpeg_quality;
    lora_flrc_preview_sig = 0;
}

static void lora_flrc_log(const char *fmt, ...)
{
    FILE *fp = fopen(LORA_FLRC_LOG_PATH, "a");
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

static void lora_flrc_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&lora_flrc_lock);
    va_start(ap, fmt);
    vsnprintf(lora_flrc_status, sizeof(lora_flrc_status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&lora_flrc_lock);
}

static void lora_flrc_append_log(const char *fmt, ...)
{
    char line[160];
    char combined[sizeof(lora_flrc_log_text)];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&lora_flrc_lock);
    snprintf(combined, sizeof(combined), "%s\n%s", line, lora_flrc_log_text);
    snprintf(lora_flrc_log_text, sizeof(lora_flrc_log_text), "%s", combined);
    pthread_mutex_unlock(&lora_flrc_lock);
    lora_flrc_log("%s", line);
}

static void lora_flrc_reset_counters_locked(lora_flrc_mode_t mode)
{
    lora_flrc_mode = mode;
    lora_flrc_stop_requested = 0;
    lora_flrc_start_us = ui_monotonic_us();
    lora_flrc_packets = 0;
    lora_flrc_bytes = 0;
    lora_flrc_errors = 0;
    lora_flrc_last_stats_log_us = 0;
    lora_flrc_last_state = RADIOLIB_ERR_NONE;
    lora_flrc_last_rssi_avg = 0.0f;
    lora_flrc_last_rssi_sync = 0.0f;
}

static int16_t lora_flrc_begin_radio(void)
{
    int16_t state;
    int16_t first_state;
    uint8_t flrc_sync[] = {0x2D, 0x01, 0x4B, 0x1D};

    if(lora_hw_prepare() != 0) {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }
    if(lora_create_radio_candidate(LORA_CHIP_LR2021) != 0 || !lora_lr2021) {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }

    lora_lr2021->irqDioNum = LORA_LR2021_IRQ_DIO_NUM;
    first_state = lora_lr2021->beginFLRC(lora_flrc_freq_mhz,
                                         (uint16_t)lora_flrc_bitrate_kbps,
                                         RADIOLIB_LR2021_FLRC_CR_3_4,
                                         LORA_FLRC_DEFAULT_POWER, 16,
                                         RADIOLIB_SHAPING_0_5, 3.0f);
    lora_flrc_log("beginFLRC freq=%.1f br=%u power=%d tcxo=3.0 state=%d %s",
                  lora_flrc_freq_mhz, lora_flrc_bitrate_kbps,
                  LORA_FLRC_DEFAULT_POWER, first_state,
                  lora_error_name(first_state));
    if(lora_lr2021_should_retry_xtal(first_state)) {
        first_state = lora_lr2021->beginFLRC(lora_flrc_freq_mhz,
                                             (uint16_t)lora_flrc_bitrate_kbps,
                                             RADIOLIB_LR2021_FLRC_CR_3_4,
                                             LORA_FLRC_DEFAULT_POWER, 16,
                                             RADIOLIB_SHAPING_0_5, 0.0f);
        lora_flrc_log("beginFLRC retry XTAL tcxo=0 state=%d %s",
                      first_state, lora_error_name(first_state));
    }
    if(first_state != RADIOLIB_ERR_NONE &&
       first_state != RADIOLIB_ERR_SPI_CMD_INVALID) {
        return first_state;
    }
    if(first_state == RADIOLIB_ERR_SPI_CMD_INVALID) {
        lora_flrc_log("beginFLRC power stage returned %d; continue with manual FLRC tail config for LR2021 16E8",
                      first_state);
    }

    lora_lr2021_apply_16e8_rf_switch();
    state = lora_lr2021_set_16e8_hf_power(LORA_FLRC_DEFAULT_POWER);
    lora_flrc_log("FLRC setOutputPower %d state=%d %s",
                  LORA_FLRC_DEFAULT_POWER, state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setPreambleLength(16);
    lora_flrc_log("FLRC setPreambleLength 16 state=%d %s",
                  state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setDataShaping(RADIOLIB_SHAPING_0_5);
    lora_flrc_log("FLRC setDataShaping 0.5 state=%d %s",
                  state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setSyncWord(flrc_sync, sizeof(flrc_sync));
    lora_flrc_log("FLRC setSyncWord 2D014B1D state=%d %s",
                  state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->fixedPacketLengthMode(LORA_FLRC_PAYLOAD_LEN);
    lora_flrc_log("FLRC fixedPacketLength len=%u state=%d %s",
                  (unsigned)LORA_FLRC_PAYLOAD_LEN,
                  state, lora_error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = lora_lr2021->setCRC(2);
    lora_flrc_log("FLRC setCRC 2 state=%d %s", state, lora_error_name(state));
    return state;
}

static int16_t lora_flrc_fast_transmit(uint8_t *payload, size_t len)
{
    int16_t state = lora_lr2021->startTransmit(payload, len);
    uint64_t start_us;
    uint64_t timeout_us;

    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    timeout_us = ((uint64_t)len * 8ULL * 1000ULL) /
                 (uint64_t)(lora_flrc_bitrate_kbps ?
                            lora_flrc_bitrate_kbps : LORA_FLRC_DEFAULT_BR);
    timeout_us = timeout_us * 6ULL + 20000ULL;
    if(timeout_us < 30000ULL) {
        timeout_us = 30000ULL;
    }

    start_us = ui_monotonic_us();
    while(!lora_hal->digitalRead(LORA_PIN_DIO1)) {
        if(ui_monotonic_us() - start_us > timeout_us) {
            (void)lora_lr2021->finishTransmit();
            return RADIOLIB_ERR_TX_TIMEOUT;
        }
        sched_yield();
    }

    return lora_lr2021->finishTransmit();
}

static int16_t lora_flrc_start_continuous_rx(void)
{
    return lora_lr2021->startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF,
                                     RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
                                     RADIOLIB_IRQ_RX_DEFAULT_MASK,
                                     LORA_FLRC_PAYLOAD_LEN);
}

static int lora_flrc_should_stop(void)
{
    int stop;

    pthread_mutex_lock(&lora_flrc_lock);
    stop = lora_flrc_stop_requested;
    pthread_mutex_unlock(&lora_flrc_lock);
    return stop;
}

static void lora_flrc_record_packet(size_t len, int16_t state, int ok,
                                    float rssi_avg, float rssi_sync)
{
    pthread_mutex_lock(&lora_flrc_lock);
    if(ok) {
        lora_flrc_packets++;
        lora_flrc_bytes += len;
        lora_flrc_last_rssi_avg = rssi_avg;
        lora_flrc_last_rssi_sync = rssi_sync;
    } else if(state != RADIOLIB_ERR_RX_TIMEOUT) {
        lora_flrc_errors++;
        lora_flrc_last_state = state;
    }
    pthread_mutex_unlock(&lora_flrc_lock);
}

static void *lora_flrc_worker_cb(void *arg)
{
    lora_flrc_mode_t mode = (lora_flrc_mode_t)(intptr_t)arg;
    uint8_t payload[LORA_FLRC_PAYLOAD_LEN];
    uint32_t seq = 0;
    int16_t state;

    memset(payload, 0xA5, sizeof(payload));
    state = lora_flrc_begin_radio();
    if(state != RADIOLIB_ERR_NONE) {
        lora_flrc_set_status("LR2021 FLRC init failed: %d %s",
                             state, lora_error_name(state));
        lora_flrc_append_log("init failed state=%d %s", state,
                             lora_error_name(state));
        pthread_mutex_lock(&lora_flrc_lock);
        lora_flrc_worker_active = 0;
        lora_flrc_mode = LORA_FLRC_MODE_IDLE;
        pthread_mutex_unlock(&lora_flrc_lock);
        return NULL;
    }

    lora_flrc_set_status("%s running %.1f MHz %u kbps",
                         mode == LORA_FLRC_MODE_TX ? "TX" : "RX",
                         lora_flrc_freq_mhz, lora_flrc_bitrate_kbps);
    lora_flrc_append_log("%s started %.1f MHz %u kbps len=%u",
                         mode == LORA_FLRC_MODE_TX ? "TX" : "RX",
                         lora_flrc_freq_mhz, lora_flrc_bitrate_kbps,
                         (unsigned)LORA_FLRC_PAYLOAD_LEN);

    while(!lora_flrc_should_stop()) {
        if(mode == LORA_FLRC_MODE_TX) {
            seq++;
            memcpy(payload, &seq, sizeof(seq));
            state = lora_flrc_fast_transmit(payload, sizeof(payload));
            lora_flrc_record_packet(sizeof(payload), state,
                                    state == RADIOLIB_ERR_NONE, 0.0f, 0.0f);
            if(state != RADIOLIB_ERR_NONE) {
                lora_flrc_append_log("TX state=%d %s", state,
                                     lora_error_name(state));
                lora_hal->delay(20);
            }
        } else {
            float rssi_avg = 0.0f;
            float rssi_sync = 0.0f;
            uint16_t packet_len = 0;

            state = lora_flrc_start_continuous_rx();
            if(state != RADIOLIB_ERR_NONE) {
                lora_flrc_record_packet(0, state, 0, 0.0f, 0.0f);
                lora_flrc_append_log("RX start state=%d %s", state,
                                     lora_error_name(state));
                lora_hal->delay(10);
                continue;
            }

            while(!lora_flrc_should_stop()) {
                if(!lora_hal->digitalRead(LORA_PIN_DIO1)) {
                    usleep(LORA_FLRC_RX_POLL_US);
                    continue;
                }

                packet_len = LORA_FLRC_PAYLOAD_LEN;
                state = lora_lr2021->readData(payload, sizeof(payload));
                if(state == RADIOLIB_ERR_NONE) {
                    lora_flrc_record_packet(packet_len, state, 1, rssi_avg,
                                            rssi_sync);
                } else {
                    lora_flrc_record_packet(0, state, 0, 0.0f, 0.0f);
                    (void)lora_lr2021->finishReceive();
                    lora_hal->delayMicroseconds(250);
                    state = lora_flrc_start_continuous_rx();
                    if(state != RADIOLIB_ERR_NONE) {
                        lora_flrc_record_packet(0, state, 0, 0.0f, 0.0f);
                        lora_flrc_append_log("RX restart state=%d %s", state,
                                             lora_error_name(state));
                        lora_hal->delay(10);
                        break;
                    }
                }
            }
        }
    }

    if(lora_lr2021) {
        (void)lora_lr2021->standby();
    }
    lora_flrc_set_status("Stopped");
    lora_flrc_append_log("%s stopped", mode == LORA_FLRC_MODE_TX ? "TX" : "RX");

    pthread_mutex_lock(&lora_flrc_lock);
    lora_flrc_worker_active = 0;
    lora_flrc_mode = LORA_FLRC_MODE_IDLE;
    pthread_mutex_unlock(&lora_flrc_lock);
    return NULL;
}

static void lora_flrc_start(lora_flrc_mode_t mode)
{
    pthread_t thread;

    pthread_mutex_lock(&lora_flrc_lock);
    if(lora_flrc_worker_active) {
        pthread_mutex_unlock(&lora_flrc_lock);
        lora_flrc_set_status("FLRC busy");
        return;
    }
    lora_flrc_reset_counters_locked(mode);
    lora_flrc_worker_active = 1;
    pthread_mutex_unlock(&lora_flrc_lock);

    if(pthread_create(&thread, NULL, lora_flrc_worker_cb,
                      (void *)(intptr_t)mode) != 0) {
        pthread_mutex_lock(&lora_flrc_lock);
        lora_flrc_worker_active = 0;
        lora_flrc_mode = LORA_FLRC_MODE_IDLE;
        pthread_mutex_unlock(&lora_flrc_lock);
        lora_flrc_set_status("Thread start failed");
        return;
    }
    pthread_detach(thread);
}

static void lora_flrc_request_stop(void)
{
    pthread_mutex_lock(&lora_flrc_lock);
    lora_flrc_stop_requested = 1;
    pthread_mutex_unlock(&lora_flrc_lock);
}

static void lora_flrc_stop_wait(void)
{
    lora_flrc_request_stop();
    for(int i = 0; i < 50; i++) {
        int active;

        pthread_mutex_lock(&lora_flrc_lock);
        active = lora_flrc_worker_active;
        pthread_mutex_unlock(&lora_flrc_lock);
        if(!active) {
            break;
        }
        usleep(40000);
    }
}

static void lora_flrc_video_read_log_tail(char *out, size_t out_len)
{
    FILE *fp;
    char lines[10][128];
    char line[128];
    unsigned index = 0;
    unsigned count = 0;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    fp = fopen(LORA_FLRC_VIDEO_LOG_PATH, "r");
    if(!fp) {
        snprintf(out, out_len, "No video log yet: %s", LORA_FLRC_VIDEO_LOG_PATH);
        return;
    }
    memset(lines, 0, sizeof(lines));
    while(fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while(len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
            line[--len] = '\0';
        }
        snprintf(lines[index], sizeof(lines[index]), "%s", line);
        index = (index + 1U) % (unsigned)(sizeof(lines) / sizeof(lines[0]));
        if(count < (unsigned)(sizeof(lines) / sizeof(lines[0]))) {
            count++;
        }
    }
    fclose(fp);

    for(unsigned i = 0; i < count; i++) {
        unsigned pos = (index + i) % (unsigned)(sizeof(lines) / sizeof(lines[0]));
        if(lines[pos][0]) {
            strncat(out, lines[pos], out_len - strlen(out) - 1U);
            if(i + 1U < count) {
                strncat(out, "\n", out_len - strlen(out) - 1U);
            }
        }
    }
}

static uint64_t lora_flrc_scan_ull_after(const char *line, const char *key,
                                         uint64_t fallback)
{
    const char *p = strstr(line, key);
    unsigned long long value;

    if(!p) {
        return fallback;
    }
    p += strlen(key);
    if(sscanf(p, "%llu", &value) == 1) {
        return (uint64_t)value;
    }
    return fallback;
}

static double lora_flrc_scan_double_after(const char *line, const char *key,
                                          double fallback)
{
    const char *p = strstr(line, key);
    double value;

    if(!p) {
        return fallback;
    }
    p += strlen(key);
    if(sscanf(p, "%lf", &value) == 1) {
        return value;
    }
    return fallback;
}

static void lora_flrc_video_parse_log_stats(void)
{
    FILE *fp = fopen(LORA_FLRC_VIDEO_LOG_PATH, "r");
    char line[256];
    double mbps = 0.0;
    double file_mbps = 0.0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    uint64_t frames = 0;
    uint64_t dropped = 0;
    uint64_t elapsed_ms = 0;

    if(!fp) {
        return;
    }
    while(fgets(line, sizeof(line), fp)) {
        if(strncmp(line, "STATS ", 6) == 0 ||
           strncmp(line, "RESULT ", 7) == 0) {
            mbps = lora_flrc_scan_double_after(line, "mbps=", mbps);
            file_mbps = lora_flrc_scan_double_after(line, "file_mbps=",
                                                    file_mbps);
            packets = lora_flrc_scan_ull_after(line, "packets=", packets);
            bytes = lora_flrc_scan_ull_after(line, "bytes=", bytes);
            bytes = lora_flrc_scan_ull_after(line, "bytes_air=", bytes);
            errors = lora_flrc_scan_ull_after(line, "errors=", errors);
            frames = lora_flrc_scan_ull_after(line, "frames=", frames);
            dropped = lora_flrc_scan_ull_after(line, "dropped=", dropped);
            elapsed_ms = lora_flrc_scan_ull_after(line, "elapsed_ms=",
                                                  elapsed_ms);
        } else if(strncmp(line, "QUEUE_TX_FRAME ", 15) == 0) {
            frames = lora_flrc_scan_ull_after(line, "sent=", frames);
            errors = lora_flrc_scan_ull_after(line, "failed=", errors);
            dropped = lora_flrc_scan_ull_after(line, "dropped=", dropped);
        } else if(strncmp(line, "STREAM2_TX_FRAME ", 17) == 0) {
            frames = lora_flrc_scan_ull_after(line, "id=", frames);
            packets = lora_flrc_scan_ull_after(line, "packets=", packets);
            errors = lora_flrc_scan_ull_after(line, "errors=", errors);
            dropped = lora_flrc_scan_ull_after(line, "dropped=", dropped);
        } else if(strncmp(line, "STREAM2_RX_FRAME ", 17) == 0) {
            frames = lora_flrc_scan_ull_after(line, "completed=", frames);
            dropped = lora_flrc_scan_ull_after(line, "dropped=", dropped);
        }
    }
    fclose(fp);

    pthread_mutex_lock(&lora_flrc_lock);
    lora_flrc_video_mbps = mbps;
    lora_flrc_video_file_mbps = file_mbps;
    lora_flrc_video_packets = packets;
    lora_flrc_video_bytes = bytes;
    lora_flrc_video_errors = errors;
    lora_flrc_video_frames = frames;
    lora_flrc_video_dropped = dropped;
    lora_flrc_video_elapsed_ms = elapsed_ms;
    pthread_mutex_unlock(&lora_flrc_lock);
}

static int lora_flrc_video_process_running(void)
{
    int status = 0;
    pid_t rc;

    if(lora_flrc_video_pid <= 0) {
        return 0;
    }
    rc = waitpid(lora_flrc_video_pid, &status, WNOHANG);
    if(rc == 0) {
        return 1;
    }
    if(rc == lora_flrc_video_pid) {
        if(WIFEXITED(status)) {
            lora_flrc_append_log("Video %s exited code=%d",
                                 lora_flrc_video_role,
                                 WEXITSTATUS(status));
        } else if(WIFSIGNALED(status)) {
            lora_flrc_append_log("Video %s killed signal=%d",
                                 lora_flrc_video_role,
                                 WTERMSIG(status));
        }
        lora_flrc_video_pid = -1;
        lora_flrc_video_role[0] = '\0';
        return 0;
    }
    if(errno == ECHILD) {
        lora_flrc_video_pid = -1;
        lora_flrc_video_role[0] = '\0';
    }
    return 0;
}

static void lora_flrc_run_shell(const char *cmd)
{
    int rc;

    if(!cmd || !cmd[0]) {
        return;
    }
    rc = system(cmd);
    if(rc != 0) {
        lora_flrc_log("shell rc=%d cmd=%s", rc, cmd);
    }
}

static void lora_flrc_video_stop(void)
{
    if(lora_flrc_video_pid <= 0) {
        unlink(LORA_FLRC_CAMERA_STOP_FILE);
        lora_flrc_run_shell(
            "touch " LORA_FLRC_CAMERA_STOP_FILE
            "; killall k230_lora_flrc_video k230_lora_flrc_tile_stream k230_camera_capture 2>/dev/null || true"
            "; pkill -f k230_flrc_camera_stream.sh 2>/dev/null || true");
        return;
    }
    lora_flrc_run_shell("touch " LORA_FLRC_CAMERA_STOP_FILE);
    kill(-lora_flrc_video_pid, SIGTERM);
    kill(lora_flrc_video_pid, SIGTERM);
    for(int i = 0; i < 20; i++) {
        if(!lora_flrc_video_process_running()) {
            return;
        }
        usleep(100000);
    }
    kill(-lora_flrc_video_pid, SIGKILL);
    kill(lora_flrc_video_pid, SIGKILL);
    (void)waitpid(lora_flrc_video_pid, NULL, 0);
    lora_flrc_run_shell(
        "killall k230_lora_flrc_video k230_lora_flrc_tile_stream k230_camera_capture 2>/dev/null || true"
        "; pkill -f k230_flrc_camera_stream.sh 2>/dev/null || true");
    lora_flrc_append_log("Video %s stopped", lora_flrc_video_role);
    lora_flrc_video_pid = -1;
    lora_flrc_video_role[0] = '\0';
}

static void lora_flrc_release_app_radio(void)
{
    lora_flrc_stop_wait();
    lora_delete_radio_objects();
    if(lora_hal) {
        delete lora_hal;
        lora_hal = NULL;
    }
    lora_initialized = 0;
}

static void lora_flrc_video_start(const char *role)
{
    char cmd[1024];
    char camera_transform[96];
    float freq;
    unsigned br;
    unsigned preview_w;
    unsigned preview_h;
    unsigned preview_fps;
    unsigned jpeg_quality;
    int flip_x;
    int flip_y;
    int base_rotate;
    int effective_flip_x;
    int effective_flip_y;
    pid_t pid;
    const char *ui_role;
    const char *target_path;

    if(!role || !role[0]) {
        return;
    }
    if(lora_flrc_video_process_running()) {
        lora_flrc_set_status("Video stream already running");
        return;
    }

    pthread_mutex_lock(&lora_flrc_lock);
    freq = lora_flrc_freq_mhz;
    br = lora_flrc_bitrate_kbps;
    preview_w = lora_flrc_camera_preview_w;
    preview_h = lora_flrc_camera_preview_h;
    preview_fps = lora_flrc_camera_preview_fps;
    jpeg_quality = lora_flrc_camera_jpeg_quality;
    flip_x = lora_flrc_camera_flip_x;
    flip_y = lora_flrc_camera_flip_y;
    lora_flrc_video_mbps = 0.0;
    lora_flrc_video_file_mbps = 0.0;
    lora_flrc_video_packets = 0;
    lora_flrc_video_bytes = 0;
    lora_flrc_video_errors = 0;
    lora_flrc_video_frames = 0;
    lora_flrc_video_dropped = 0;
    lora_flrc_video_elapsed_ms = 0;
    pthread_mutex_unlock(&lora_flrc_lock);

    base_rotate = ui_is_landscape() ? 0 : 90;
    effective_flip_x = flip_x;
    effective_flip_y = flip_y;
    if(ui_is_landscape()) {
        effective_flip_y = !effective_flip_y;
    }
    snprintf(camera_transform, sizeof(camera_transform), "--camera-rotate %d%s%s",
             base_rotate,
             effective_flip_x ? " --camera-flip-x" : "",
             effective_flip_y ? " --camera-flip-y" : "");

    lora_flrc_release_app_radio();
    unlink(LORA_FLRC_VIDEO_LOG_PATH);
    unlink(LORA_FLRC_CAMERA_STOP_FILE);
    unlink(LORA_FLRC_CAMERA_PREVIEW_FILE);
    unlink(LORA_FLRC_CAMERA_PREVIEW_META);
    mkdir(LORA_FLRC_VIDEO_OUT_DIR, 0755);

    ui_role = "RX";
    if(strcmp(role, "stream-tx") == 0) {
        ui_role = "TX";
        snprintf(cmd, sizeof(cmd),
                 "cd /root/app/k230_phone_ui && exec %s --role stream-tx --file '%s' --freq %.1f --br %u --len %u --spi-hz %u --power %d --rx-poll-us %u --retries %u --ack-wait-ms %u > '%s' 2>&1",
                 LORA_FLRC_VIDEO_BIN, LORA_FLRC_VIDEO_FILE, freq, br,
                 (unsigned)LORA_FLRC_PAYLOAD_LEN, (unsigned)LORA_FLRC_VIDEO_SPI_HZ,
                 LORA_FLRC_DEFAULT_POWER, (unsigned)LORA_FLRC_VIDEO_RX_POLL_US,
                 (unsigned)LORA_FLRC_VIDEO_RETRIES,
                 (unsigned)LORA_FLRC_VIDEO_ACK_WAIT_MS,
                 LORA_FLRC_VIDEO_LOG_PATH);
    } else if(strcmp(role, "stream-rx") == 0) {
        ui_role = "RX";
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p '%s'; cd /root/app/k230_phone_ui && exec %s --role stream-rx --freq %.1f --br %u --duration 180 --len %u --spi-hz %u --power %d --rx-poll-us %u --retries %u --ack-wait-ms %u --out-dir '%s' > '%s' 2>&1",
                 LORA_FLRC_VIDEO_OUT_DIR, LORA_FLRC_VIDEO_BIN, freq, br,
                 (unsigned)LORA_FLRC_PAYLOAD_LEN, (unsigned)LORA_FLRC_VIDEO_SPI_HZ,
                 LORA_FLRC_DEFAULT_POWER, (unsigned)LORA_FLRC_VIDEO_RX_POLL_US,
                 (unsigned)LORA_FLRC_VIDEO_RETRIES,
                 (unsigned)LORA_FLRC_VIDEO_ACK_WAIT_MS,
                 LORA_FLRC_VIDEO_OUT_DIR, LORA_FLRC_VIDEO_LOG_PATH);
    } else if(strcmp(role, "camera-tx") == 0) {
        ui_role = "CAM TX";
        snprintf(cmd, sizeof(cmd),
                 "cd /root/app/k230_phone_ui && exec %s --role tx --duration 3600 --stream-format image --codec h265 --capture-width 320 --capture-height 240 --encode-width %u --encode-height %u --jpeg-quality %u --segment 1 --preview-width %u --preview-height %u --preview-fps %u --preview-buffer 1 --compressed-image-stream %s --freq %.1f --br %u --spi-hz %u > '%s' 2>&1",
                 LORA_FLRC_CAMERA_STREAM_BIN, preview_w, preview_h,
                 jpeg_quality, preview_w, preview_h, preview_fps,
                 camera_transform, freq, br,
                 (unsigned)LORA_FLRC_VIDEO_SPI_HZ,
                 LORA_FLRC_VIDEO_LOG_PATH);
    } else if(strcmp(role, "camera-rx") == 0) {
        ui_role = "CAM RX";
        snprintf(cmd, sizeof(cmd),
                 "cd /root/app/k230_phone_ui && exec %s --role rx --duration 3600 --preview-width %u --preview-height %u --preview-fps %u --preview-buffer 1 --no-tile-stream --fast-frame --freq %.1f --br %u --spi-hz %u > '%s' 2>&1",
                 LORA_FLRC_CAMERA_STREAM_BIN, preview_w, preview_h,
                 preview_fps, freq, br,
                 (unsigned)LORA_FLRC_VIDEO_SPI_HZ,
                 LORA_FLRC_VIDEO_LOG_PATH);
    } else {
        lora_flrc_set_status("Unknown video role");
        return;
    }
    target_path = (strcmp(role, "stream-tx") == 0) ? LORA_FLRC_VIDEO_FILE :
                  ((strcmp(role, "stream-rx") == 0) ? LORA_FLRC_VIDEO_OUT_DIR :
                   "camera-segments");

    pid = fork();
    if(pid < 0) {
        lora_flrc_set_status("Video process start failed");
        lora_flrc_append_log("fork failed: %s", strerror(errno));
        return;
    }
    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    lora_flrc_video_pid = pid;
    snprintf(lora_flrc_video_role, sizeof(lora_flrc_video_role), "%s", ui_role);
    lora_flrc_set_status("Video %s started", lora_flrc_video_role);
    lora_flrc_append_log("Video %s start freq=%.1f br=%u spi=%u target=%s %ux%u jpeg_q=%u fps=%u",
                         lora_flrc_video_role, freq, br,
                         (unsigned)LORA_FLRC_VIDEO_SPI_HZ, target_path,
                         preview_w, preview_h, jpeg_quality, preview_fps);
}

static void lora_flrc_freq_event_cb(lv_event_t *event)
{
    float freq = (float)(intptr_t)lv_event_get_user_data(event);

    if(freq < 2000.0f) {
        freq = LORA_FLRC_DEFAULT_FREQ;
    }
    pthread_mutex_lock(&lora_flrc_lock);
    lora_flrc_freq_mhz = freq;
    pthread_mutex_unlock(&lora_flrc_lock);
}

static void lora_flrc_bitrate_event_cb(lv_event_t *event)
{
    unsigned br = (unsigned)(uintptr_t)lv_event_get_user_data(event);

    pthread_mutex_lock(&lora_flrc_lock);
    lora_flrc_bitrate_kbps = br;
    pthread_mutex_unlock(&lora_flrc_lock);
}

static void lora_flrc_camera_preset_event_cb(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);

    pthread_mutex_lock(&lora_flrc_lock);
    lora_flrc_apply_camera_preset(index);
    pthread_mutex_unlock(&lora_flrc_lock);
    lora_flrc_append_log("Camera preset %s",
                         lora_flrc_camera_presets[
                             lora_flrc_camera_preset_index].name);
}

static void lora_flrc_camera_flip_event_cb(lv_event_t *event)
{
    unsigned which = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    int flip_x;
    int flip_y;

    pthread_mutex_lock(&lora_flrc_lock);
    if(which == 0U) {
        lora_flrc_camera_flip_x = !lora_flrc_camera_flip_x;
    } else {
        lora_flrc_camera_flip_y = !lora_flrc_camera_flip_y;
    }
    flip_x = lora_flrc_camera_flip_x;
    flip_y = lora_flrc_camera_flip_y;
    pthread_mutex_unlock(&lora_flrc_lock);
    lora_flrc_append_log("Camera transform rotate=90 flip_x=%d flip_y=%d",
                         flip_x, flip_y);
}

static void lora_flrc_tx_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_video_stop();
    lora_flrc_start(LORA_FLRC_MODE_TX);
}

static void lora_flrc_rx_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_video_stop();
    lora_flrc_start(LORA_FLRC_MODE_RX);
}

static void lora_flrc_stop_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_request_stop();
    lora_flrc_video_stop();
    lora_flrc_set_status("Stopping");
}

static void lora_flrc_video_tx_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_video_start("stream-tx");
}

static void lora_flrc_video_rx_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_video_start("stream-rx");
}

static void lora_flrc_camera_tx_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_video_start("camera-tx");
}

static void lora_flrc_camera_rx_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_video_start("camera-rx");
}

static void lora_flrc_settings_close(void)
{
    if(lora_flrc_settings_overlay &&
       lv_obj_is_valid(lora_flrc_settings_overlay)) {
        lv_obj_delete(lora_flrc_settings_overlay);
    }
    lora_flrc_settings_overlay = NULL;
    memset(lora_flrc_freq_btn, 0, sizeof(lora_flrc_freq_btn));
    memset(lora_flrc_br_btn, 0, sizeof(lora_flrc_br_btn));
    memset(lora_flrc_camera_preset_btn, 0,
           sizeof(lora_flrc_camera_preset_btn));
    memset(lora_flrc_camera_flip_btn, 0, sizeof(lora_flrc_camera_flip_btn));
}

static void lora_flrc_settings_close_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_settings_close();
}

static void lora_flrc_settings_tx_event_cb(lv_event_t *event)
{
    lora_flrc_tx_event_cb(event);
    lora_flrc_settings_close();
}

static void lora_flrc_settings_rx_event_cb(lv_event_t *event)
{
    lora_flrc_rx_event_cb(event);
    lora_flrc_settings_close();
}

static void lora_flrc_settings_video_tx_event_cb(lv_event_t *event)
{
    lora_flrc_video_tx_event_cb(event);
    lora_flrc_settings_close();
}

static void lora_flrc_settings_video_rx_event_cb(lv_event_t *event)
{
    lora_flrc_video_rx_event_cb(event);
    lora_flrc_settings_close();
}

static void lora_flrc_create_settings_overlay(void)
{
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *section;
    lv_obj_t *btn;
    int sw = ui_screen_width();
    int sh = ui_screen_height();
    int landscape = ui_is_landscape();
    int card_w = landscape ? 720 : 520;
    int card_h = landscape ? 420 : 660;
    int pad = 22;
    int gap = 12;
    int col_w;
    int y;

    if(card_w > sw - 80) {
        card_w = sw - 80;
    }
    if(card_h > sh - 80) {
        card_h = sh - 80;
    }
    if(card_w < 360) {
        card_w = 360;
    }
    if(card_h < 360) {
        card_h = 360;
    }

    lora_flrc_settings_close();
    lora_flrc_settings_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(lora_flrc_settings_overlay);
    lv_obj_set_style_bg_color(lora_flrc_settings_overlay,
                              lv_color_hex(0x05080C), 0);
    lv_obj_set_style_bg_opa(lora_flrc_settings_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(lora_flrc_settings_overlay, 0, 0);
    lv_obj_clear_flag(lora_flrc_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);

    card = ui_panel(lora_flrc_settings_overlay, (sw - card_w) / 2,
                    (sh - card_h) / 2, card_w, card_h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111821), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);

    title = ui_label(card, "FLRC Settings", &lv_font_montserrat_26, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);

    btn = ui_command_button(card, card_w - pad * 2 - 92, 0, 92, "Close",
                            0xF2F5F8);
    lv_obj_add_event_cb(btn, lora_flrc_settings_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    section = ui_label(card, "Frequency", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_pos(section, 0, 58);
    col_w = (card_w - pad * 2 - gap) / 2;
    if(col_w < 140) {
        col_w = 140;
    }
    y = 92;
    lora_flrc_freq_btn[0] = ui_command_button(card, 0, y, col_w, "2400 MHz",
                                              0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_freq_btn[0], lora_flrc_freq_event_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)2400);
    lora_flrc_freq_btn[1] =
        ui_command_button(card, col_w + gap, y, col_w, "2450 MHz",
                          0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_freq_btn[1], lora_flrc_freq_event_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)2450);

    section = ui_label(card, "Bitrate", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap * 2) / 3;
    if(col_w < 100) {
        col_w = 100;
    }
    lora_flrc_br_btn[0] = ui_command_button(card, 0, y, col_w, "650k",
                                            0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_br_btn[0], lora_flrc_bitrate_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)650U);
    lora_flrc_br_btn[1] = ui_command_button(card, col_w + gap, y, col_w,
                                            "1.3M", 0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_br_btn[1], lora_flrc_bitrate_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)1300U);
    lora_flrc_br_btn[2] =
        ui_command_button(card, (col_w + gap) * 2, y, col_w, "2.6M",
                          0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_br_btn[2], lora_flrc_bitrate_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)2600U);

    section = ui_label(card, "Camera Preset", &lv_font_montserrat_18,
                       0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap * 2) / 3;
    if(col_w < 130) {
        col_w = 130;
    }
    for(unsigned i = 0;
        i < (unsigned)(sizeof(lora_flrc_camera_presets) /
                       sizeof(lora_flrc_camera_presets[0])); i++) {
        lora_flrc_camera_preset_btn[i] =
            ui_command_button(card, (col_w + gap) * (int)i, y, col_w,
                              lora_flrc_camera_presets[i].name, 0xF2F5F8);
        lv_obj_add_event_cb(lora_flrc_camera_preset_btn[i],
                            lora_flrc_camera_preset_event_cb,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    section = ui_label(card, "Camera Transform", &lv_font_montserrat_18,
                       0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap) / 2;
    if(col_w < 140) {
        col_w = 140;
    }
    lora_flrc_camera_flip_btn[0] =
        ui_command_button(card, 0, y, col_w, "H Flip", 0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_camera_flip_btn[0],
                        lora_flrc_camera_flip_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)0U);
    lora_flrc_camera_flip_btn[1] =
        ui_command_button(card, col_w + gap, y, col_w, "V Flip", 0xF2F5F8);
    lv_obj_add_event_cb(lora_flrc_camera_flip_btn[1],
                        lora_flrc_camera_flip_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)1U);

    section = ui_label(card, "Tools", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    btn = ui_command_button(card, 0, y, col_w, "Raw TX", 0x25C281);
    lv_obj_add_event_cb(btn, lora_flrc_settings_tx_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(card, col_w + gap, y, col_w, "Raw RX", 0x3DA5FF);
    lv_obj_add_event_cb(btn, lora_flrc_settings_rx_event_cb, LV_EVENT_CLICKED,
                        NULL);

    y += 72;
    btn = ui_command_button(card, 0, y, col_w, "File TX", 0x25C281);
    lv_obj_add_event_cb(btn, lora_flrc_settings_video_tx_event_cb,
                        LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(card, col_w + gap, y, col_w, "File RX", 0x3DA5FF);
    lv_obj_add_event_cb(btn, lora_flrc_settings_video_rx_event_cb,
                        LV_EVENT_CLICKED, NULL);

    lora_flrc_timer_cb(NULL);
}

static void lora_flrc_settings_event_cb(lv_event_t *event)
{
    (void)event;
    lora_flrc_create_settings_overlay();
}

static int lora_flrc_load_preview_pixels(void)
{
    FILE *fp;
    struct stat st;
    size_t bytes_read;
    uint64_t sig;
    size_t expected;

    if(stat(LORA_FLRC_CAMERA_PREVIEW_FILE, &st) != 0) {
        return 0;
    }
    for(unsigned i = 0;
        i < (unsigned)(sizeof(lora_flrc_camera_presets) /
                       sizeof(lora_flrc_camera_presets[0])); i++) {
        size_t bytes = (size_t)lora_flrc_camera_presets[i].width *
                       (size_t)lora_flrc_camera_presets[i].height * 2U;

        if(st.st_size == (off_t)bytes) {
            lora_flrc_camera_preview_w = lora_flrc_camera_presets[i].width;
            lora_flrc_camera_preview_h = lora_flrc_camera_presets[i].height;
            break;
        }
    }
    expected = lora_flrc_preview_bytes();
    if(st.st_size != (off_t)expected ||
       lora_flrc_camera_preview_w > LORA_FLRC_CAMERA_PREVIEW_W_MAX ||
       lora_flrc_camera_preview_h > LORA_FLRC_CAMERA_PREVIEW_H_MAX) {
        return 0;
    }

#if defined(__linux__)
    sig = ((uint64_t)st.st_mtim.tv_sec * 1000000000ULL) ^
          (uint64_t)st.st_mtim.tv_nsec ^
          (uint64_t)st.st_size;
#else
    sig = ((uint64_t)st.st_mtime << 32) ^ (uint64_t)st.st_size;
#endif
    if(sig == lora_flrc_preview_sig && lora_flrc_preview_dsc.data) {
        return 1;
    }

    if(!lora_flrc_preview_pixels ||
       lora_flrc_preview_dsc.data_size != expected) {
        free(lora_flrc_preview_pixels);
        lora_flrc_preview_pixels = (uint8_t *)malloc(expected);
        if(!lora_flrc_preview_pixels) {
            lora_flrc_append_log("preview alloc failed");
            return 0;
        }
        memset(&lora_flrc_preview_dsc, 0, sizeof(lora_flrc_preview_dsc));
    }

    fp = fopen(LORA_FLRC_CAMERA_PREVIEW_FILE, "rb");
    if(!fp) {
        return 0;
    }
    bytes_read = fread(lora_flrc_preview_pixels, 1, expected, fp);
    fclose(fp);
    if(bytes_read != expected) {
        return 0;
    }

    memset(&lora_flrc_preview_dsc, 0, sizeof(lora_flrc_preview_dsc));
    lora_flrc_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lora_flrc_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    lora_flrc_preview_dsc.header.w = lora_flrc_camera_preview_w;
    lora_flrc_preview_dsc.header.h = lora_flrc_camera_preview_h;
    lora_flrc_preview_dsc.header.stride = lora_flrc_camera_preview_w * 2U;
    lora_flrc_preview_dsc.data_size = expected;
    lora_flrc_preview_dsc.data = lora_flrc_preview_pixels;
    lora_flrc_preview_sig = sig;
    return 1;
}

static void lora_flrc_set_preview_hint(const char *text)
{
    if(lora_flrc_preview_placeholder &&
       lv_obj_is_valid(lora_flrc_preview_placeholder)) {
        lv_obj_clear_flag(lora_flrc_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    if(lora_flrc_preview_placeholder_label &&
       lv_obj_is_valid(lora_flrc_preview_placeholder_label)) {
        lv_label_set_text(lora_flrc_preview_placeholder_label, ui_tr(text));
    }
    if(lora_flrc_preview_image && lv_obj_is_valid(lora_flrc_preview_image)) {
        lv_obj_add_flag(lora_flrc_preview_image, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lora_flrc_update_preview(int camera_preview_active)
{
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t scale;
    int avail_w;
    int avail_h;
    int have_frame;

    if(!lora_flrc_preview_image ||
       !lv_obj_is_valid(lora_flrc_preview_image)) {
        return;
    }

    have_frame = lora_flrc_load_preview_pixels();
    if(!camera_preview_active && !have_frame) {
        lora_flrc_set_preview_hint("Start Cam TX/RX to preview camera video");
        return;
    }

    if(!have_frame) {
        lora_flrc_set_preview_hint("Waiting for FLRC camera frames");
        return;
    }

    avail_w = lora_flrc_preview_panel_w - 24;
    avail_h = lora_flrc_preview_panel_h - 24;
    if(avail_w < 120) {
        avail_w = 120;
    }
    if(avail_h < 90) {
        avail_h = 90;
    }
    scale_x = (uint32_t)((uint64_t)avail_w * LV_SCALE_NONE /
                         lora_flrc_camera_preview_w);
    scale_y = (uint32_t)((uint64_t)avail_h * LV_SCALE_NONE /
                         lora_flrc_camera_preview_h);
    scale = scale_x < scale_y ? scale_x : scale_y;
    if(scale < 64U) {
        scale = 64U;
    }

    lv_image_cache_drop(&lora_flrc_preview_dsc);
    lv_image_set_src(lora_flrc_preview_image, &lora_flrc_preview_dsc);
    lv_image_set_scale(lora_flrc_preview_image, scale);
    lv_obj_align(lora_flrc_preview_image, LV_ALIGN_CENTER, 0, 16);
    lv_obj_clear_flag(lora_flrc_preview_image, LV_OBJ_FLAG_HIDDEN);
    if(lora_flrc_preview_placeholder &&
       lv_obj_is_valid(lora_flrc_preview_placeholder)) {
        lv_obj_add_flag(lora_flrc_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(lora_flrc_preview_image);
    app_request_fast_refresh();
}

static void lora_flrc_style_choice(lv_obj_t *btn, int active, uint32_t color)
{
    if(!btn || !lv_obj_is_valid(btn)) {
        return;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? color : 0x222832), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(active ? color : 0x2A3037),
                                  0);
}

static void lora_flrc_timer_cb(lv_timer_t *timer)
{
    char status[160];
    char log_text[sizeof(lora_flrc_log_text)];
    uint64_t packets;
    uint64_t bytes;
    uint64_t errors;
    uint64_t start_us;
    int16_t last_state;
    float rssi_avg;
    float rssi_sync;
    float freq;
    unsigned br;
    unsigned preview_w;
    unsigned preview_h;
    unsigned preview_fps;
    unsigned jpeg_quality;
    int flip_x;
    int flip_y;
    double video_mbps;
    double video_file_mbps;
    uint64_t video_packets;
    uint64_t video_bytes;
    uint64_t video_errors;
    uint64_t video_frames;
    uint64_t video_dropped;
    uint64_t video_elapsed_ms;
    lora_flrc_mode_t mode;
    double elapsed_s;
    double mbps;
    int video_running;
    int camera_preview_active;
    char video_role[16];
    char video_log[640];

    (void)timer;
    video_running = lora_flrc_video_process_running();
    snprintf(video_role, sizeof(video_role), "%s", lora_flrc_video_role);
    camera_preview_active = video_running &&
                            (strcmp(video_role, "CAM RX") == 0 ||
                             strcmp(video_role, "CAM TX") == 0);
    if(video_running || video_role[0]) {
        lora_flrc_video_read_log_tail(video_log, sizeof(video_log));
        lora_flrc_video_parse_log_stats();
    } else {
        video_log[0] = '\0';
    }

    pthread_mutex_lock(&lora_flrc_lock);
    snprintf(status, sizeof(status), "%s", lora_flrc_status);
    snprintf(log_text, sizeof(log_text), "%s", lora_flrc_log_text);
    packets = lora_flrc_packets;
    bytes = lora_flrc_bytes;
    errors = lora_flrc_errors;
    start_us = lora_flrc_start_us;
    last_state = lora_flrc_last_state;
    rssi_avg = lora_flrc_last_rssi_avg;
    rssi_sync = lora_flrc_last_rssi_sync;
    freq = lora_flrc_freq_mhz;
    br = lora_flrc_bitrate_kbps;
    preview_w = lora_flrc_camera_preview_w;
    preview_h = lora_flrc_camera_preview_h;
    preview_fps = lora_flrc_camera_preview_fps;
    jpeg_quality = lora_flrc_camera_jpeg_quality;
    flip_x = lora_flrc_camera_flip_x;
    flip_y = lora_flrc_camera_flip_y;
    video_mbps = lora_flrc_video_mbps;
    video_file_mbps = lora_flrc_video_file_mbps;
    video_packets = lora_flrc_video_packets;
    video_bytes = lora_flrc_video_bytes;
    video_errors = lora_flrc_video_errors;
    video_frames = lora_flrc_video_frames;
    video_dropped = lora_flrc_video_dropped;
    video_elapsed_ms = lora_flrc_video_elapsed_ms;
    mode = lora_flrc_mode;
    pthread_mutex_unlock(&lora_flrc_lock);

    elapsed_s = start_us ? (double)(ui_monotonic_us() - start_us) / 1000000.0 :
                0.0;
    mbps = elapsed_s > 0.05 ? ((double)bytes * 8.0) / elapsed_s / 1000000.0 :
           0.0;

    if(lora_flrc_status_label && lv_obj_is_valid(lora_flrc_status_label)) {
        if(video_running) {
            lv_label_set_text_fmt(lora_flrc_status_label, "Video %s running",
                                  video_role);
        } else {
            lv_label_set_text(lora_flrc_status_label, ui_tr(status));
        }
    }
    if(lora_flrc_config_label && lv_obj_is_valid(lora_flrc_config_label)) {
        lv_label_set_text_fmt(lora_flrc_config_label,
                              "LR2021 FLRC  %.1f MHz  %u kbps  CR3/4  PWR %d dBm",
                              freq, br, LORA_FLRC_DEFAULT_POWER);
    }
    if(lora_flrc_stats_label && lv_obj_is_valid(lora_flrc_stats_label)) {
        if(video_running) {
            if(camera_preview_active) {
                lv_label_set_text_fmt(lora_flrc_stats_label,
                                      "JPEG camera %s  %ux%u Q%u @%ufps  Cam H%d V%d\nAir %.3f Mbps  File %.3f Mbps\nFrames %llu  Drop %llu  Err %llu  %.1fs",
                                      strcmp(video_role, "CAM TX") == 0 ?
                                      "TX" : "RX",
                                      preview_w, preview_h, jpeg_quality,
                                      preview_fps, flip_x, flip_y, video_mbps,
                                      video_file_mbps,
                                      (unsigned long long)video_frames,
                                      (unsigned long long)video_dropped,
                                      (unsigned long long)video_errors,
                                      video_elapsed_ms / 1000.0);
            } else {
                lv_label_set_text_fmt(lora_flrc_stats_label,
                                      "Reliable video stream\n%s  file=%s\nCRC32 + NACK repair, retries %u",
                                      video_role,
                                      strcmp(video_role, "TX") == 0 ?
                                      LORA_FLRC_VIDEO_FILE :
                                      LORA_FLRC_VIDEO_OUT_DIR,
                                      (unsigned)LORA_FLRC_VIDEO_RETRIES);
            }
        } else {
            lv_label_set_text_fmt(lora_flrc_stats_label,
                                  "%s  %.2f Mbps\nPackets %llu  Bytes %llu  Errors %llu\nRSSI avg %.1f  sync %.1f  last %d",
                                  mode == LORA_FLRC_MODE_TX ? "TX" :
                                  (mode == LORA_FLRC_MODE_RX ? "RX" : "Idle"),
                                  mbps, (unsigned long long)packets,
                                  (unsigned long long)bytes,
                                  (unsigned long long)errors,
                                  rssi_avg, rssi_sync, last_state);
        }
    }
    if(lora_flrc_log_label && lv_obj_is_valid(lora_flrc_log_label)) {
        lv_label_set_text(lora_flrc_log_label,
                          video_running && video_log[0] ? video_log : log_text);
    }

    lora_flrc_update_preview(camera_preview_active);

    if(mode != LORA_FLRC_MODE_IDLE &&
       (lora_flrc_last_stats_log_us == 0 ||
        ui_monotonic_us() - lora_flrc_last_stats_log_us >= 1000000ULL)) {
        lora_flrc_last_stats_log_us = ui_monotonic_us();
        lora_flrc_log("STATS mode=%s mbps=%.4f packets=%llu bytes=%llu errors=%llu last=%d freq=%.1f br=%u len=%u",
                      mode == LORA_FLRC_MODE_TX ? "TX" :
                      (mode == LORA_FLRC_MODE_RX ? "RX" : "IDLE"),
                      mbps,
                      (unsigned long long)packets,
                      (unsigned long long)bytes,
                      (unsigned long long)errors,
                      last_state,
                      freq,
                      br,
                      (unsigned)LORA_FLRC_PAYLOAD_LEN);
    }

    lora_flrc_style_choice(lora_flrc_freq_btn[0], freq < 2425.0f, 0x7C3AED);
    lora_flrc_style_choice(lora_flrc_freq_btn[1], freq >= 2425.0f, 0x7C3AED);
    lora_flrc_style_choice(lora_flrc_br_btn[0], br == 650U, 0x25C281);
    lora_flrc_style_choice(lora_flrc_br_btn[1], br == 1300U, 0x25C281);
    lora_flrc_style_choice(lora_flrc_br_btn[2], br == 2600U, 0x25C281);
    for(unsigned i = 0;
        i < (unsigned)(sizeof(lora_flrc_camera_preset_btn) /
                       sizeof(lora_flrc_camera_preset_btn[0])); i++) {
        lora_flrc_style_choice(lora_flrc_camera_preset_btn[i],
                               i == lora_flrc_camera_preset_index, 0x3DA5FF);
    }
    lora_flrc_style_choice(lora_flrc_camera_flip_btn[0], flip_x, 0x25C281);
    lora_flrc_style_choice(lora_flrc_camera_flip_btn[1], flip_y, 0x25C281);
}

void ui_lora_flrc_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *preview;
    lv_obj_t *preview_title;
    lv_obj_t *preview_icon;
    lv_obj_t *side;
    lv_obj_t *btn;
    int landscape = ui_is_landscape();
    int screen_w = ui_screen_width();
    int body_h = ui_body_height(144);
    int margin = landscape ? 24 : 24;
    int gap = landscape ? 22 : 18;
    int body_w = screen_w - margin * 2;
    int preview_x = margin;
    int preview_y = landscape ? 20 : 20;
    int preview_w;
    int preview_h;
    int side_x;
    int side_y;
    int side_w;
    int side_h;
    int side_inner_w;
    int row_w;
    int button_y;
    int log_y;

    memset(lora_flrc_freq_btn, 0, sizeof(lora_flrc_freq_btn));
    memset(lora_flrc_br_btn, 0, sizeof(lora_flrc_br_btn));
    memset(lora_flrc_camera_preset_btn, 0,
           sizeof(lora_flrc_camera_preset_btn));
    memset(lora_flrc_camera_flip_btn, 0, sizeof(lora_flrc_camera_flip_btn));
    lora_flrc_apply_camera_preset(lora_flrc_camera_preset_index);
    lora_flrc_preview_image = NULL;
    lora_flrc_preview_placeholder = NULL;
    lora_flrc_preview_placeholder_label = NULL;
    lora_flrc_preview_panel_w = 0;
    lora_flrc_preview_panel_h = 0;

    ui_create_header(scr, "LoRa FLRC");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    if(landscape) {
        preview_h = body_h - 40;
        if(preview_h < 320) {
            preview_h = 320;
        }
        preview_w = body_w * 58 / 100;
        if(preview_w > body_w - 360 - gap) {
            preview_w = body_w - 360 - gap;
        }
        if(preview_w < 420) {
            preview_w = body_w - 340 - gap;
        }
        if(preview_w < 320) {
            preview_w = 320;
        }
        side_x = preview_x + preview_w + gap;
        side_y = preview_y;
        side_w = screen_w - side_x - margin;
        side_h = preview_h;
    } else {
        preview_w = body_w;
        preview_h = 420;
        if(preview_h > body_h / 2) {
            preview_h = body_h / 2;
        }
        if(preview_h < 300) {
            preview_h = 300;
        }
        side_x = margin;
        side_y = preview_y + preview_h + gap;
        side_w = preview_w;
        side_h = body_h - side_y - 32;
        if(side_h < 470) {
            side_h = 470;
        }
    }

    preview = ui_panel(body, preview_x, preview_y, preview_w, preview_h);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(preview, 0, 0);

    preview_title = ui_label(preview, "FLRC Preview", &lv_font_montserrat_24,
                             0xF2F5F8);
    lv_obj_set_pos(preview_title, 18, 14);
    lora_flrc_preview_panel_w = preview_w - 32;
    lora_flrc_preview_panel_h = preview_h - 72;

    lora_flrc_preview_image = lv_image_create(preview);
    lv_obj_add_flag(lora_flrc_preview_image, LV_OBJ_FLAG_HIDDEN);

    lora_flrc_preview_placeholder = lv_obj_create(preview);
    lv_obj_set_pos(lora_flrc_preview_placeholder, 16, 56);
    lv_obj_set_size(lora_flrc_preview_placeholder, preview_w - 32,
                    preview_h - 72);
    lv_obj_set_style_bg_color(lora_flrc_preview_placeholder,
                              lv_color_hex(0x17202B), 0);
    lv_obj_set_style_bg_opa(lora_flrc_preview_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lora_flrc_preview_placeholder, 0, 0);
    lv_obj_set_style_radius(lora_flrc_preview_placeholder, 8, 0);
    lv_obj_clear_flag(lora_flrc_preview_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    preview_icon = ui_label(lora_flrc_preview_placeholder, LV_SYMBOL_VIDEO,
                            &lv_font_montserrat_32, 0x3DA5FF);
    lv_obj_align(preview_icon, LV_ALIGN_CENTER, 0, -36);
    lora_flrc_preview_placeholder_label =
        ui_label(lora_flrc_preview_placeholder,
                 "Start Cam RX to preview received video",
                 &lv_font_montserrat_18, 0xC9D3DF);
    lv_obj_set_width(lora_flrc_preview_placeholder_label,
                     preview_w > 72 ? preview_w - 72 : preview_w);
    lv_obj_set_style_text_align(lora_flrc_preview_placeholder_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lora_flrc_preview_placeholder_label, LV_ALIGN_CENTER, 0, 28);

    side = ui_panel(body, side_x, side_y, side_w, side_h);
    lv_obj_set_style_bg_color(side, lv_color_hex(0x151B22), 0);
    side_inner_w = side_w - 32;
    if(side_inner_w < 260) {
        side_inner_w = 260;
    }

    ui_label(side, "Camera FLRC", &lv_font_montserrat_24, 0xF2F5F8);
    lora_flrc_status_label =
        ui_label(side, "Ready", &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_pos(lora_flrc_status_label, 0, 42);
    lv_obj_set_width(lora_flrc_status_label, side_inner_w);
    lv_label_set_long_mode(lora_flrc_status_label, LV_LABEL_LONG_DOT);

    lora_flrc_config_label =
        ui_label(side, "--", &lv_font_montserrat_16, 0xC9D3DF);
    lv_obj_set_pos(lora_flrc_config_label, 0, 74);
    lv_obj_set_width(lora_flrc_config_label, side_inner_w);
    lv_label_set_long_mode(lora_flrc_config_label, LV_LABEL_LONG_DOT);

    lora_flrc_stats_label =
        ui_label(side, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(lora_flrc_stats_label, 0, 104);
    lv_obj_set_width(lora_flrc_stats_label, side_inner_w);
    lv_label_set_long_mode(lora_flrc_stats_label, LV_LABEL_LONG_WRAP);

    button_y = landscape ? 180 : 176;
    row_w = (side_inner_w - gap) / 2;
    if(row_w < 118) {
        row_w = 118;
    }
    btn = ui_command_button(side, 0, button_y, row_w, "Cam RX", 0x3DA5FF);
    lv_obj_add_event_cb(btn, lora_flrc_camera_rx_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(side, row_w + gap, button_y, row_w, "Cam TX",
                            0x25C281);
    lv_obj_add_event_cb(btn, lora_flrc_camera_tx_event_cb, LV_EVENT_CLICKED,
                        NULL);
    button_y += 74;
    btn = ui_command_button(side, 0, button_y, row_w, "Stop", 0xEF4D5A);
    lv_obj_add_event_cb(btn, lora_flrc_stop_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(side, row_w + gap, button_y, row_w, "Settings",
                            0xF2F5F8);
    lv_obj_add_event_cb(btn, lora_flrc_settings_event_cb, LV_EVENT_CLICKED,
                        NULL);

    log_y = button_y + 88;
    if(landscape && log_y < side_h - 130) {
        log_y = side_h - 130;
    }
    lora_flrc_log_label =
        ui_label(side, lora_flrc_log_text, &lv_font_montserrat_16, 0xC9D3DF);
    lv_obj_set_pos(lora_flrc_log_label, 0, log_y);
    lv_obj_set_width(lora_flrc_log_label, side_inner_w);
    lv_label_set_long_mode(lora_flrc_log_label, LV_LABEL_LONG_WRAP);

    if(!lora_flrc_timer) {
        lora_flrc_timer = lv_timer_create(lora_flrc_timer_cb, 250, NULL);
    }
    lora_flrc_timer_cb(NULL);
}

void ui_lora_flrc_cleanup(void)
{
    if(lora_flrc_timer) {
        lv_timer_delete(lora_flrc_timer);
        lora_flrc_timer = NULL;
    }
    lora_flrc_video_stop();
    lora_flrc_stop_wait();
    lora_flrc_settings_close();
    lora_delete_radio_objects();
    if(lora_hal) {
        delete lora_hal;
        lora_hal = NULL;
    }
    lora_initialized = 0;
    lora_flrc_status_label = NULL;
    lora_flrc_config_label = NULL;
    lora_flrc_stats_label = NULL;
    lora_flrc_log_label = NULL;
    lora_flrc_preview_image = NULL;
    lora_flrc_preview_placeholder = NULL;
    lora_flrc_preview_placeholder_label = NULL;
    lora_flrc_settings_overlay = NULL;
    memset(lora_flrc_freq_btn, 0, sizeof(lora_flrc_freq_btn));
    memset(lora_flrc_br_btn, 0, sizeof(lora_flrc_br_btn));
    memset(lora_flrc_camera_preset_btn, 0,
           sizeof(lora_flrc_camera_preset_btn));
    memset(lora_flrc_camera_flip_btn, 0, sizeof(lora_flrc_camera_flip_btn));
}
