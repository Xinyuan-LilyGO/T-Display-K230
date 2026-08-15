#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <string>
#include <vector>

#include "modules/LR2021/LR2021.h"
#include "modules/SX126x/SX1262.h"

#define PROBE_VERSION "0.20"
#define LORA_SPI_DEV "/dev/spidev0.0"
#define LORA_SPI_SPEED_HZ 4000000U
#define LORA_PIN_CS 14U
#define LORA_PIN_RST 5U
#define LORA_PIN_BUSY 19U
#define LORA_PIN_DIO1 20U
#define LORA_LR2021_IRQ_DIO_NUM 11U
#define LORA_PIN_POWER 44U
#define MESHTASTIC_HEADER_LENGTH 16U
#define MESHTASTIC_MAX_LORA_PAYLOAD_LEN 255U
#define MESHTASTIC_DATA_PAYLOAD_LEN 233U
#define MESHTASTIC_NODENUM_BROADCAST 0xFFFFFFFFU
#define MESHTASTIC_TEXT_MESSAGE_APP 1U
#define MESHTASTIC_POSITION_APP 3U
#define MESHTASTIC_NODEINFO_APP 4U
#define MESHTASTIC_ROUTING_APP 5U
#define MESHTASTIC_ADMIN_APP 6U
#define MESHTASTIC_TELEMETRY_APP 67U
#define MESHTASTIC_NEIGHBORINFO_APP 71U
#define MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK 0x07U
#define MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK 0x08U
#define MESHTASTIC_PACKET_FLAGS_HOP_START_MASK 0xE0U
#define MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT 5U
#define MESHTASTIC_ROUTING_ERROR_NONE 0U
#define MESHTASTIC_SYNC_WORD 0x2BU
#define MESHTASTIC_DEFAULT_REGION "US"
#define MESHTASTIC_DEFAULT_PRESET "LONG_FAST"
#define MESHTASTIC_DEFAULT_SOCKET_PATH "/tmp/k230_meshtastic.sock"
#define MESHTASTIC_MAX_IPC_MESSAGE_LEN 220U
#define MESHTASTIC_EVENT_LOG_LINES 32U
#define MESHTASTIC_EVENT_LOG_LINE_LEN 192U
#define MESHTASTIC_CHAT_LOG_LINES 24U
#define MESHTASTIC_CHAT_LOG_LINE_LEN 192U
#define MESHTASTIC_NODE_CACHE_SIZE 24U
#define MESHTASTIC_PACKET_HISTORY_SIZE 64U
#define MESHTASTIC_PACKET_HISTORY_TTL_US (30ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_CHAT_DEDUP_SIZE 16U
#define MESHTASTIC_CHAT_DEDUP_TTL_US (45ULL * 1000000ULL)
#define MESHTASTIC_DELAYED_TX_QUEUE_SIZE 4U
#define MESHTASTIC_REBROADCAST_MIN_DELAY_US 150000ULL
#define MESHTASTIC_REBROADCAST_JITTER_US 700000ULL
#define MESHTASTIC_ACK_RESPONSE_DELAY_US 5500000ULL
#define MESHTASTIC_ACK_RETRY_QUEUE_SIZE 4U
#define MESHTASTIC_ACK_RETRY_TIMEOUT_US 12000000ULL
#define MESHTASTIC_ACK_RETRY_MAX 3U
#define MESHTASTIC_NODEINFO_INTERVAL_US (10ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_NODEINFO_RETRY_US (60ULL * 1000000ULL)
#define MESHTASTIC_PHONEAPI_UART_DEV "/dev/ttyS1"
#define MESHTASTIC_PHONEAPI_ADV_REFRESH_US (15ULL * 1000000ULL)
#define MESHTASTIC_PHONEAPI_CONFIG_NONCE 69420U
#define MESHTASTIC_PHONEAPI_NODEINFO_NONCE 69421U
#define MESHTASTIC_HW_MODEL_NRF52840_PCA10059 40U
#define MESHTASTIC_MAX_K230_TX_POWER_DBM 22
#define OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH 0
#define OVERRIDE_SLOT_PRESET_HASH -1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const uint32_t lr2021_16e8_rf_switch_dio_pins[] = {
    RADIOLIB_LR2021_DIO6, RADIOLIB_LR2021_DIO7,
    RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC,
};

static const Module::RfSwitchMode_t lr2021_16e8_rf_switch_table[] = {
    {LR2021::MODE_STBY, {0, 0}},
    {LR2021::MODE_TX, {0, 0}},
    {LR2021::MODE_RX, {0, 0}},
    {LR2021::MODE_TX_HF, {0, 1}},
    {LR2021::MODE_RX_HF, {1, 0}},
    END_OF_MODE_TABLE,
};

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
        request_interrupt_pin(interrupt_num, interrupt_cb, mode);
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
        gpiod_request_config_set_consumer(request_config,
                                          "k230-meshtastic-probe");

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
        gpiod_request_config_set_consumer(request_config,
                                          "k230-meshtastic-probe-irq");
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

typedef enum {
    CHIP_NONE = 0,
    CHIP_SX1262,
    CHIP_LR2021,
} chip_type_t;

typedef enum {
    OP_IDLE = 0,
    OP_RX,
    OP_TX,
} radio_op_t;

typedef struct {
    float freq = 915.0f;
    float bandwidth = 125.0f;
    uint8_t sf = 12;
    uint8_t cr = 5;
    uint8_t sync_word = 0xCD;
    int8_t power = 22;
    uint16_t preamble = 16;
} probe_profile_t;

typedef struct {
    probe_profile_t profile;
    std::string spi_path = LORA_SPI_DEV;
    std::string node_name;
    std::string send_once;
    std::string message = "hello from k230";
    std::string channel_name;
    std::string psk = "default";
    std::string socket_path = MESHTASTIC_DEFAULT_SOCKET_PATH;
    std::string client_send;
    bool auto_tx = false;
    bool mesh_mode = false;
    bool daemon_mode = false;
    bool client_send_requested = false;
    bool client_status = false;
    bool client_log = false;
    bool client_chat = false;
    bool client_nodes = false;
    bool client_quit = false;
    bool rebroadcast = true;
    bool advertise_nodeinfo = true;
    bool want_ack = false;
    bool want_ack_set = false;
    uint32_t from_node = 0;
    uint32_t to_node = MESHTASTIC_NODENUM_BROADCAST;
    uint32_t packet_id = 0;
    uint8_t hop_limit = 3;
    uint32_t interval_ms = 1000;
    uint32_t duration_sec = 0;
    uint32_t nodeinfo_interval_sec =
        (uint32_t)(MESHTASTIC_NODEINFO_INTERVAL_US / 1000000ULL);
    std::string region;
    std::string preset;
    uint32_t frequency_slot = 0;
    uint32_t resolved_slot = 0;
    uint32_t resolved_slot_count = 0;
    std::string resolved_region;
    std::string resolved_preset;
    bool manual_freq = false;
    bool manual_bw = false;
    bool manual_sf = false;
    bool manual_cr = false;
    bool manual_sw = false;
    bool manual_power = false;
    bool manual_preamble = false;
} probe_options_t;

typedef struct {
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t flags;
    uint8_t channel;
    uint8_t next_hop;
    uint8_t relay_node;
} mesh_header_t;

typedef struct {
    uint32_t from;
    uint32_t id;
    uint8_t channel;
    uint64_t seen_us;
} mesh_history_entry_t;

typedef struct {
    uint32_t from;
    uint32_t to;
    uint64_t seen_us;
    char text[96];
} mesh_chat_dedup_entry_t;

typedef struct {
    uint32_t node = 0;
    uint64_t last_seen_us = 0;
    int rssi_dbm = 0;
    float snr = 0.0f;
    uint32_t rx_count = 0;
    char long_name[40] = {0};
    char short_name[8] = {0};
    int hw_model = -1;
    bool has_position = false;
    bool has_altitude = false;
    bool has_ground_speed = false;
    bool has_ground_track = false;
    int32_t latitude_i = 0;
    int32_t longitude_i = 0;
    int32_t altitude_m = 0;
    uint32_t ground_speed_cms = 0;
    uint32_t ground_track_1e5 = 0;
    uint32_t sats_in_view = 0;
    uint32_t precision_bits = 0;
    uint32_t position_timestamp = 0;
    bool has_device_metrics = false;
    bool has_battery_level = false;
    bool has_device_voltage = false;
    bool has_channel_utilization = false;
    bool has_air_util_tx = false;
    uint32_t battery_level = 0;
    uint32_t uptime_seconds = 0;
    float device_voltage = 0.0f;
    float channel_utilization = 0.0f;
    float air_util_tx = 0.0f;
    bool has_environment_metrics = false;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_pressure = false;
    bool has_environment_voltage = false;
    bool has_iaq = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    float pressure_hpa = 0.0f;
    float environment_voltage = 0.0f;
    uint32_t iaq = 0;
    uint32_t telemetry_timestamp = 0;
    bool has_neighbor_info = false;
    uint32_t neighbor_node_id = 0;
    uint32_t neighbor_last_sent_by_id = 0;
    uint32_t neighbor_broadcast_interval_secs = 0;
    uint32_t neighbor_count = 0;
    char neighbor_summary[160] = {0};
} mesh_node_entry_t;

typedef struct {
    std::vector<uint8_t> bytes;
    std::string summary;
    bool rebroadcast = false;
    bool want_ack = false;
    bool routing_ack = false;
    uint32_t rebroadcast_from = 0;
    uint32_t to_node = 0;
    uint32_t from_node = 0;
    uint32_t packet_id = 0;
    uint32_t ack_request_id = 0;
    uint8_t channel = 0;
    uint8_t old_hop = 0;
    uint8_t new_hop = 0;
} tx_frame_t;

typedef struct {
    tx_frame_t frame;
    uint64_t due_us = 0;
    bool active = false;
} delayed_tx_t;

typedef struct {
    tx_frame_t frame;
    uint64_t due_us = 0;
    uint32_t to_node = 0;
    uint32_t packet_id = 0;
    uint8_t retries_left = 0;
    bool active = false;
} ack_retry_entry_t;

typedef struct {
    uint32_t portnum = 0;
    std::vector<uint8_t> payload;
    uint32_t request_id = 0;
    uint32_t reply_id = 0;
    bool want_response = false;
} mesh_data_proto_t;

typedef struct {
    bool active = false;
    uint32_t to_node = MESHTASTIC_NODENUM_BROADCAST;
    uint32_t from_node = 0;
    uint32_t packet_id = 0;
    uint8_t hop_limit = 3;
    bool want_ack = false;
    mesh_data_proto_t data;
} phoneapi_mesh_tx_t;

typedef struct {
    std::string long_name;
    std::string short_name;
    int hw_model = -1;
    bool has_name = false;
} mesh_user_info_t;

typedef struct {
    bool has_latitude = false;
    bool has_longitude = false;
    bool has_altitude = false;
    bool has_ground_speed = false;
    bool has_ground_track = false;
    int32_t latitude_i = 0;
    int32_t longitude_i = 0;
    int32_t altitude_m = 0;
    uint32_t ground_speed_cms = 0;
    uint32_t ground_track_1e5 = 0;
    uint32_t sats_in_view = 0;
    uint32_t precision_bits = 0;
    uint32_t timestamp = 0;
} mesh_position_info_t;

typedef struct {
    bool has_device_metrics = false;
    bool has_battery_level = false;
    bool has_device_voltage = false;
    bool has_channel_utilization = false;
    bool has_air_util_tx = false;
    uint32_t battery_level = 0;
    uint32_t uptime_seconds = 0;
    float device_voltage = 0.0f;
    float channel_utilization = 0.0f;
    float air_util_tx = 0.0f;
    bool has_environment_metrics = false;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_pressure = false;
    bool has_environment_voltage = false;
    bool has_iaq = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    float pressure_hpa = 0.0f;
    float environment_voltage = 0.0f;
    uint32_t iaq = 0;
    uint32_t timestamp = 0;
} mesh_telemetry_info_t;

typedef struct {
    uint32_t node_id = 0;
    float snr = 0.0f;
    uint32_t last_rx_time = 0;
    uint32_t broadcast_interval_secs = 0;
    bool has_node_id = false;
    bool has_snr = false;
} mesh_neighbor_entry_info_t;

typedef struct {
    uint32_t node_id = 0;
    uint32_t last_sent_by_id = 0;
    uint32_t broadcast_interval_secs = 0;
    uint32_t neighbor_count = 0;
    std::string summary;
    bool has_neighbor_info = false;
} mesh_neighbor_info_t;

typedef enum {
    PHONEAPI_BRIDGE_OFFLINE = 0,
    PHONEAPI_BRIDGE_PROBING,
    PHONEAPI_BRIDGE_READY,
    PHONEAPI_BRIDGE_CONNECTED,
    PHONEAPI_BRIDGE_UNSUPPORTED,
    PHONEAPI_BRIDGE_ERROR,
} phoneapi_bridge_state_t;

typedef enum {
    REGION_PROFILE_STD = 0,
    REGION_PROFILE_EU868,
    REGION_PROFILE_LITE,
    REGION_PROFILE_NARROW,
    REGION_PROFILE_HAM_20KHZ,
    REGION_PROFILE_HAM_100KHZ,
} region_profile_type_t;

typedef struct {
    const char *name;
    float spacing;
    float padding;
} meshtastic_region_profile_t;

typedef struct {
    const char *name;
    const char *display_name;
    float bandwidth;
    float wide_bandwidth;
    uint8_t sf;
    uint8_t cr;
} meshtastic_preset_t;

typedef struct {
    const char *name;
    float freq_start;
    float freq_end;
    int power_limit;
    bool wide_lora;
    region_profile_type_t profile;
    const char *default_preset;
    int16_t override_slot;
} meshtastic_region_t;

static const meshtastic_region_profile_t region_profiles[] = {
    {"STD", 0.0f, 0.0f},
    {"EU868", 0.0f, 0.0f},
    {"LITE", 0.4f, 0.0375f},
    {"NARROW", 0.0f, 0.0104f},
    {"HAM_20KHZ", 0.0f, 0.0022f},
    {"HAM_100KHZ", 0.0f, 0.01875f},
};

static const meshtastic_preset_t meshtastic_presets[] = {
    {"LONG_FAST", "LongFast", 250.0f, 812.5f, 11, 5},
    {"LONG_SLOW", "LongSlow", 125.0f, 406.25f, 12, 8},
    {"LONG_MODERATE", "LongMod", 125.0f, 406.25f, 11, 8},
    {"LONG_TURBO", "LongTurbo", 500.0f, 1625.0f, 11, 8},
    {"MEDIUM_SLOW", "MediumSlow", 250.0f, 812.5f, 10, 5},
    {"MEDIUM_FAST", "MediumFast", 250.0f, 812.5f, 9, 5},
    {"MEDIUM_TURBO", "MediumTurbo", 500.0f, 1625.0f, 9, 5},
    {"SHORT_SLOW", "ShortSlow", 250.0f, 812.5f, 8, 5},
    {"SHORT_FAST", "ShortFast", 250.0f, 812.5f, 7, 5},
    {"SHORT_TURBO", "ShortTurbo", 500.0f, 1625.0f, 7, 5},
    {"LITE_FAST", "LiteFast", 125.0f, 125.0f, 9, 5},
    {"LITE_SLOW", "LiteSlow", 125.0f, 125.0f, 10, 5},
    {"NARROW_FAST", "NarrowFast", 62.5f, 62.5f, 7, 6},
    {"NARROW_SLOW", "NarrowSlow", 62.5f, 62.5f, 8, 6},
    {"TINY_FAST", "TinyFast", 15.6f, 15.6f, 7, 5},
    {"TINY_SLOW", "TinySlow", 15.6f, 15.6f, 8, 6},
};

static const meshtastic_region_t meshtastic_regions[] = {
    {"US", 902.0f, 928.0f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"EU_433", 433.0f, 434.0f, 10, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"EU_868", 869.4f, 869.65f, 27, false, REGION_PROFILE_EU868, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"EU_866", 865.6f, 867.6f, 27, false, REGION_PROFILE_LITE, "LITE_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"EU_N_868", 869.4f, 869.65f, 27, false, REGION_PROFILE_NARROW, "NARROW_SLOW", 1},
    {"CN", 470.0f, 510.0f, 19, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"JP", 920.5f, 923.5f, 13, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"ANZ", 915.0f, 928.0f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"ANZ_433", 433.05f, 434.79f, 14, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"RU", 868.7f, 869.2f, 20, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"KR", 920.0f, 923.0f, 23, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"TW", 920.0f, 925.0f, 27, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"IN", 865.0f, 867.0f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"NZ_865", 864.0f, 868.0f, 36, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"TH", 920.0f, 925.0f, 27, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"UA_433", 433.0f, 434.7f, 10, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"MY_433", 433.0f, 435.0f, 20, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"MY_919", 919.0f, 924.0f, 27, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"SG_923", 917.0f, 925.0f, 20, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"PH_433", 433.0f, 434.7f, 10, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"PH_868", 868.0f, 869.4f, 14, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"PH_915", 915.0f, 918.0f, 24, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"KZ_433", 433.075f, 434.775f, 10, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"KZ_863", 863.0f, 868.0f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"NP_865", 865.0f, 868.0f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"BR_902", 902.0f, 907.5f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"ITU1_2M", 144.0f, 146.0f, 30, false, REGION_PROFILE_HAM_20KHZ, "TINY_FAST", 26},
    {"ITU2_2M", 144.0f, 148.0f, 30, false, REGION_PROFILE_HAM_20KHZ, "TINY_FAST", 51},
    {"ITU3_2M", 144.0f, 148.0f, 30, false, REGION_PROFILE_HAM_20KHZ, "TINY_FAST", 33},
    {"ITU2_125CM", 220.0f, 225.0f, 30, false, REGION_PROFILE_HAM_100KHZ, "NARROW_SLOW", 37},
    {"ITU1_70CM", 430.0f, 440.0f, 30, false, REGION_PROFILE_HAM_100KHZ, "NARROW_SLOW", 37},
    {"ITU2_70CM", 420.0f, 450.0f, 30, false, REGION_PROFILE_HAM_100KHZ, "NARROW_SLOW", 137},
    {"ITU3_70CM", 430.0f, 450.0f, 30, false, REGION_PROFILE_HAM_100KHZ, "NARROW_SLOW", 37},
    {"LORA_24", 2400.0f, 2483.5f, 10, true, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
    {"UNSET", 902.0f, 928.0f, 30, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
};

static const uint8_t default_psk[] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static volatile sig_atomic_t running = 1;
static volatile unsigned int radio_event_count;
static radio_op_t active_op = OP_IDLE;
static uint64_t active_op_start_us;
static size_t active_tx_len;
static uint32_t tx_count;
static uint32_t rx_count;
static uint32_t seq_count;
static uint32_t mesh_duplicate_count;
static uint32_t mesh_rebroadcast_count;
static uint32_t mesh_rebroadcast_drop_count;
static uint32_t mesh_ack_rx_count;
static uint32_t mesh_nak_rx_count;
static uint32_t mesh_ack_retry_count;
static uint32_t mesh_ack_timeout_count;
static uint32_t mesh_ack_drop_count;
static uint32_t mesh_nodeinfo_tx_count;
static uint32_t mesh_nodeinfo_drop_count;
static uint64_t mesh_next_nodeinfo_us;
static LR2021 *active_lr2021;
static mesh_history_entry_t mesh_history[MESHTASTIC_PACKET_HISTORY_SIZE];
static size_t mesh_history_count;
static size_t mesh_history_next;
static mesh_chat_dedup_entry_t mesh_chat_dedup[MESHTASTIC_CHAT_DEDUP_SIZE];
static size_t mesh_chat_dedup_next;
static delayed_tx_t mesh_delayed_tx_queue[MESHTASTIC_DELAYED_TX_QUEUE_SIZE];
static ack_retry_entry_t mesh_ack_retry_queue[MESHTASTIC_ACK_RETRY_QUEUE_SIZE];
static char daemon_event_log[MESHTASTIC_EVENT_LOG_LINES][MESHTASTIC_EVENT_LOG_LINE_LEN];
static size_t daemon_event_log_count;
static char daemon_chat_log[MESHTASTIC_CHAT_LOG_LINES][MESHTASTIC_CHAT_LOG_LINE_LEN];
static size_t daemon_chat_log_count;
static mesh_node_entry_t mesh_nodes[MESHTASTIC_NODE_CACHE_SIZE];
static size_t mesh_node_count;
static pthread_mutex_t daemon_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t phoneapi_thread;
static volatile bool phoneapi_thread_running;
static bool phoneapi_thread_started;
static probe_options_t phoneapi_opts;
static pthread_mutex_t phoneapi_uart_mutex = PTHREAD_MUTEX_INITIALIZER;
static int phoneapi_uart_fd = -1;
static pthread_mutex_t phoneapi_tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static phoneapi_mesh_tx_t phoneapi_pending_tx;
static pthread_mutex_t phoneapi_opts_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool phoneapi_opts_update_available;
static bool phoneapi_reconfigure_requested;
static pthread_mutex_t phoneapi_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static phoneapi_bridge_state_t phoneapi_bridge_state = PHONEAPI_BRIDGE_OFFLINE;
static char phoneapi_bridge_detail[160] = "not-started";
static bool phoneapi_init_sent;
static uint64_t phoneapi_last_adv_us;

static void radio_event_isr(void)
{
    (void)__sync_fetch_and_add(&radio_event_count, 1U);
}

static unsigned int take_radio_events(void)
{
    return __sync_lock_test_and_set(&radio_event_count, 0U);
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t tx_poll_finish_delay_us(size_t len)
{
    uint64_t delay = 2500000ULL + (uint64_t)len * 35000ULL;

    if(delay < 2500000ULL) {
        delay = 2500000ULL;
    }
    if(delay > 12000000ULL) {
        delay = 12000000ULL;
    }
    return delay;
}

static uint64_t tx_min_finish_delay_us(size_t len)
{
    return tx_poll_finish_delay_us(len);
}

static bool elapsed_after(uint64_t now_us, uint64_t start_us,
                          uint64_t delay_us)
{
    return now_us >= start_us && now_us - start_us > delay_us;
}

static void signal_handler(int signum)
{
    (void)signum;
    running = 0;
}

static void daemon_event(const char *fmt, ...)
{
    char line[MESHTASTIC_EVENT_LOG_LINE_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&daemon_log_mutex);
    if(daemon_event_log_count < MESHTASTIC_EVENT_LOG_LINES) {
        snprintf(daemon_event_log[daemon_event_log_count++],
                 MESHTASTIC_EVENT_LOG_LINE_LEN, "%s", line);
    } else {
        memmove(daemon_event_log, daemon_event_log + 1,
                sizeof(daemon_event_log[0]) *
                (MESHTASTIC_EVENT_LOG_LINES - 1U));
        snprintf(daemon_event_log[MESHTASTIC_EVENT_LOG_LINES - 1U],
                 MESHTASTIC_EVENT_LOG_LINE_LEN, "%s", line);
    }
    pthread_mutex_unlock(&daemon_log_mutex);
    printf("%s\n", line);
    fflush(stdout);
}

static const char *phoneapi_bridge_state_name(phoneapi_bridge_state_t state)
{
    switch(state) {
    case PHONEAPI_BRIDGE_PROBING:
        return "probing";
    case PHONEAPI_BRIDGE_READY:
        return "ready";
    case PHONEAPI_BRIDGE_CONNECTED:
        return "connected";
    case PHONEAPI_BRIDGE_UNSUPPORTED:
        return "unsupported";
    case PHONEAPI_BRIDGE_ERROR:
        return "error";
    case PHONEAPI_BRIDGE_OFFLINE:
    default:
        return "offline";
    }
}

static void phoneapi_bridge_set_state(phoneapi_bridge_state_t state,
                                      const char *fmt, ...)
{
    char detail[sizeof(phoneapi_bridge_detail)];
    va_list ap;

    if(fmt && fmt[0]) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    } else {
        snprintf(detail, sizeof(detail), "%s",
                 phoneapi_bridge_state_name(state));
    }
    for(size_t i = 0; detail[i]; i++) {
        if(isspace((unsigned char)detail[i])) {
            detail[i] = '_';
        }
    }

    pthread_mutex_lock(&phoneapi_state_mutex);
    phoneapi_bridge_state = state;
    snprintf(phoneapi_bridge_detail, sizeof(phoneapi_bridge_detail), "%s",
             detail);
    pthread_mutex_unlock(&phoneapi_state_mutex);
}

static phoneapi_bridge_state_t phoneapi_bridge_get_state(char *detail,
                                                         size_t detail_len)
{
    phoneapi_bridge_state_t state;

    pthread_mutex_lock(&phoneapi_state_mutex);
    state = phoneapi_bridge_state;
    if(detail && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", phoneapi_bridge_detail);
    }
    pthread_mutex_unlock(&phoneapi_state_mutex);
    return state;
}

static bool phoneapi_bridge_can_send(void)
{
    phoneapi_bridge_state_t state = phoneapi_bridge_get_state(nullptr, 0);

    return state == PHONEAPI_BRIDGE_READY ||
           state == PHONEAPI_BRIDGE_CONNECTED;
}

static void phoneapi_store_runtime_opts(const probe_options_t &opts,
                                        bool request_reconfigure)
{
    pthread_mutex_lock(&phoneapi_opts_mutex);
    phoneapi_opts = opts;
    phoneapi_opts_update_available = true;
    if(request_reconfigure) {
        phoneapi_reconfigure_requested = true;
    }
    pthread_mutex_unlock(&phoneapi_opts_mutex);
}

static bool phoneapi_take_runtime_opts(probe_options_t *opts,
                                       bool *request_reconfigure)
{
    bool changed = false;

    if(!opts || !request_reconfigure) {
        return false;
    }
    pthread_mutex_lock(&phoneapi_opts_mutex);
    if(phoneapi_opts_update_available || phoneapi_reconfigure_requested) {
        *opts = phoneapi_opts;
        *request_reconfigure = phoneapi_reconfigure_requested;
        phoneapi_opts_update_available = false;
        phoneapi_reconfigure_requested = false;
        changed = true;
    }
    pthread_mutex_unlock(&phoneapi_opts_mutex);
    return changed;
}

static bool phoneapi_bridge_status_line(const std::string &line)
{
    return line.rfind("+MESH:STATUS", 0) == 0 ||
           line.rfind("+MESH:ADV", 0) == 0 ||
           line.rfind("+MESH:CONNECTED", 0) == 0 ||
           line.rfind("+MESH:DISCONNECTED", 0) == 0 ||
           line.find("MESH_ADV=") != std::string::npos;
}

static bool phoneapi_bridge_status_connected(const std::string &line)
{
    if(line.rfind("+MESH:CONNECTED", 0) == 0) {
        return true;
    }
    return line.find("CONN=1") != std::string::npos ||
           line.find("MESH_CONN=1") != std::string::npos;
}

static bool phoneapi_status_bool_field(const std::string &line,
                                       const char *key,
                                       bool *present)
{
    const char *p;
    size_t key_len;

    if(present) {
        *present = false;
    }
    if(!key || !key[0]) {
        return false;
    }
    key_len = strlen(key);
    p = line.c_str();
    while((p = strstr(p, key)) != nullptr) {
        if((p == line.c_str() || p[-1] == ',' || p[-1] == ' ' ||
            p[-1] == ':') && p[key_len] == '=') {
            const char *value = p + key_len + 1U;

            if(present) {
                *present = true;
            }
            return value[0] == '1';
        }
        p += key_len;
    }
    return false;
}

static bool phoneapi_bridge_status_advertising(const std::string &line,
                                               bool *known)
{
    bool present = false;
    bool adv = false;

    if(known) {
        *known = true;
    }
    if(line.rfind("+MESH:ADV,1", 0) == 0 ||
       line.rfind("+MESH:CONNECTED", 0) == 0) {
        return true;
    }
    if(line.rfind("+MESH:ADV,0", 0) == 0) {
        return false;
    }
    adv = phoneapi_status_bool_field(line, "ADV", &present);
    if(!present) {
        adv = phoneapi_status_bool_field(line, "MESH_ADV", &present);
    }
    if(known) {
        *known = present;
    }
    return adv;
}

static std::string phoneapi_default_node_name(const probe_options_t &opts)
{
    char tmp[24];
    uint32_t suffix = opts.from_node;

    if(suffix == 0U) {
        suffix = 0x2300U;
    }
    snprintf(tmp, sizeof(tmp), "k230-%04x", (unsigned)(suffix & 0xffffU));
    return std::string(tmp);
}

static std::string phoneapi_sanitize_adv_name(const std::string &name)
{
    std::string out;

    for(size_t i = 0; i < name.size() && out.size() < 31U; i++) {
        unsigned char c = (unsigned char)name[i];
        if(c >= 0x20U && c < 0x7fU && c != ',' && c != '\r' && c != '\n') {
            out.push_back((char)c);
        } else if(!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while(!out.empty() && (out.back() == ' ' || out.back() == '-')) {
        out.pop_back();
    }
    while(!out.empty() && (out.front() == ' ' || out.front() == '-')) {
        out.erase(out.begin());
    }
    return out;
}

static std::string phoneapi_adv_name(const probe_options_t &opts)
{
    std::string name = phoneapi_sanitize_adv_name(opts.node_name);

    if(name.empty() || name == "nRF52840" ||
       name == "K230 nRF52840 AT" || name == "k230-t-display") {
        name = phoneapi_default_node_name(opts);
    }
    return name;
}

static std::string mesh_clean_text(const std::string &text)
{
    std::string out;

    for(size_t i = 0; i < text.size() && out.size() < 140U; i++) {
        unsigned char c = (unsigned char)text[i];
        if(c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else if(c >= 32U) {
            out.push_back((char)c);
        }
    }
    while(!out.empty() && isspace((unsigned char)out.back())) {
        out.pop_back();
    }
    return out;
}

static void daemon_chat(const char *fmt, ...)
{
    char line[MESHTASTIC_CHAT_LOG_LINE_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&daemon_log_mutex);
    if(daemon_chat_log_count < MESHTASTIC_CHAT_LOG_LINES) {
        snprintf(daemon_chat_log[daemon_chat_log_count++],
                 MESHTASTIC_CHAT_LOG_LINE_LEN, "%s", line);
    } else {
        memmove(daemon_chat_log, daemon_chat_log + 1,
                sizeof(daemon_chat_log[0]) *
                (MESHTASTIC_CHAT_LOG_LINES - 1U));
        snprintf(daemon_chat_log[MESHTASTIC_CHAT_LOG_LINES - 1U],
                 MESHTASTIC_CHAT_LOG_LINE_LEN, "%s", line);
    }
    pthread_mutex_unlock(&daemon_log_mutex);
}

static mesh_node_entry_t *mesh_node_get_or_create(uint32_t node)
{
    size_t slot = MESHTASTIC_NODE_CACHE_SIZE;
    size_t oldest = 0;

    if(node == 0U) {
        return nullptr;
    }
    for(size_t i = 0; i < mesh_node_count; i++) {
        if(mesh_nodes[i].node == node) {
            return &mesh_nodes[i];
        }
        if(mesh_nodes[i].last_seen_us < mesh_nodes[oldest].last_seen_us) {
            oldest = i;
        }
    }
    if(slot == MESHTASTIC_NODE_CACHE_SIZE) {
        if(mesh_node_count < MESHTASTIC_NODE_CACHE_SIZE) {
            slot = mesh_node_count++;
        } else {
            slot = oldest;
        }
        memset(&mesh_nodes[slot], 0, sizeof(mesh_nodes[slot]));
        mesh_nodes[slot].node = node;
        mesh_nodes[slot].hw_model = -1;
    }
    return &mesh_nodes[slot];
}

static void mesh_node_seen(uint32_t node, float rssi, float snr)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry) {
        return;
    }
    entry->last_seen_us = monotonic_us();
    entry->rssi_dbm = (int)roundf(rssi);
    entry->snr = snr;
    entry->rx_count++;
}

static void mesh_node_update_user(uint32_t node, const mesh_user_info_t &user)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);
    std::string clean_long = mesh_clean_text(user.long_name);
    std::string clean_short = mesh_clean_text(user.short_name);

    if(!entry) {
        return;
    }
    if(!clean_long.empty()) {
        snprintf(entry->long_name, sizeof(entry->long_name), "%s",
                 clean_long.c_str());
    }
    if(!clean_short.empty()) {
        snprintf(entry->short_name, sizeof(entry->short_name), "%s",
                 clean_short.c_str());
    }
    if(user.hw_model >= 0) {
        entry->hw_model = user.hw_model;
    }
}

static void mesh_node_update_position(uint32_t node,
                                      const mesh_position_info_t &position)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry || !position.has_latitude || !position.has_longitude) {
        return;
    }
    entry->has_position = true;
    entry->latitude_i = position.latitude_i;
    entry->longitude_i = position.longitude_i;
    entry->precision_bits = position.precision_bits;
    entry->sats_in_view = position.sats_in_view;
    entry->position_timestamp = position.timestamp;
    if(position.has_altitude) {
        entry->has_altitude = true;
        entry->altitude_m = position.altitude_m;
    }
    if(position.has_ground_speed) {
        entry->has_ground_speed = true;
        entry->ground_speed_cms = position.ground_speed_cms;
    }
    if(position.has_ground_track) {
        entry->has_ground_track = true;
        entry->ground_track_1e5 = position.ground_track_1e5;
    }
}

static void mesh_node_update_telemetry(uint32_t node,
                                       const mesh_telemetry_info_t &telemetry)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry) {
        return;
    }
    entry->telemetry_timestamp = telemetry.timestamp;
    if(telemetry.has_device_metrics) {
        entry->has_device_metrics = true;
        entry->uptime_seconds = telemetry.uptime_seconds;
        if(telemetry.has_battery_level) {
            entry->has_battery_level = true;
            entry->battery_level = telemetry.battery_level;
        }
        if(telemetry.has_device_voltage) {
            entry->has_device_voltage = true;
            entry->device_voltage = telemetry.device_voltage;
        }
        if(telemetry.has_channel_utilization) {
            entry->has_channel_utilization = true;
            entry->channel_utilization = telemetry.channel_utilization;
        }
        if(telemetry.has_air_util_tx) {
            entry->has_air_util_tx = true;
            entry->air_util_tx = telemetry.air_util_tx;
        }
    }
    if(telemetry.has_environment_metrics) {
        entry->has_environment_metrics = true;
        if(telemetry.has_temperature) {
            entry->has_temperature = true;
            entry->temperature_c = telemetry.temperature_c;
        }
        if(telemetry.has_humidity) {
            entry->has_humidity = true;
            entry->humidity_percent = telemetry.humidity_percent;
        }
        if(telemetry.has_pressure) {
            entry->has_pressure = true;
            entry->pressure_hpa = telemetry.pressure_hpa;
        }
        if(telemetry.has_environment_voltage) {
            entry->has_environment_voltage = true;
            entry->environment_voltage = telemetry.environment_voltage;
        }
        if(telemetry.has_iaq) {
            entry->has_iaq = true;
            entry->iaq = telemetry.iaq;
        }
    }
}

static void mesh_node_update_neighbor_info(uint32_t node,
                                           const mesh_neighbor_info_t &info)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry || !info.has_neighbor_info) {
        return;
    }
    entry->has_neighbor_info = true;
    entry->neighbor_node_id = info.node_id;
    entry->neighbor_last_sent_by_id = info.last_sent_by_id;
    entry->neighbor_broadcast_interval_secs =
        info.broadcast_interval_secs;
    entry->neighbor_count = info.neighbor_count;
    snprintf(entry->neighbor_summary, sizeof(entry->neighbor_summary), "%s",
             info.summary.empty() ? "-" : info.summary.c_str());
}

static const char *chip_name(chip_type_t chip)
{
    switch(chip) {
    case CHIP_SX1262:
        return "SX1262";
    case CHIP_LR2021:
        return "LR2021";
    case CHIP_NONE:
    default:
        return "none";
    }
}

static const char *op_name(radio_op_t op)
{
    switch(op) {
    case OP_RX:
        return "rx";
    case OP_TX:
        return "tx";
    case OP_IDLE:
    default:
        return "idle";
    }
}

static const char *error_name(int16_t state)
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

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
    dst[2] = (uint8_t)((value >> 16) & 0xffU);
    dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint32_t djb2_hash(const char *text)
{
    uint32_t hash = 5381U;
    int c;

    if(!text) {
        return hash;
    }
    while((c = *text++) != 0) {
        hash = ((hash << 5) + hash) + (uint8_t)c;
    }
    return hash;
}

static std::string normalize_token(const char *text)
{
    std::string out;

    if(!text) {
        return out;
    }
    while(*text) {
        unsigned char c = (unsigned char)*text++;
        if(isalnum(c)) {
            out.push_back((char)toupper(c));
        }
    }
    return out;
}

static const meshtastic_preset_t *find_meshtastic_preset(const char *name)
{
    std::string needle = normalize_token(name);

    if(needle.empty()) {
        return nullptr;
    }
    for(size_t i = 0; i < ARRAY_SIZE(meshtastic_presets); i++) {
        if(normalize_token(meshtastic_presets[i].name) == needle ||
           normalize_token(meshtastic_presets[i].display_name) == needle) {
            return &meshtastic_presets[i];
        }
    }
    return nullptr;
}

static const meshtastic_region_t *find_meshtastic_region(const char *name)
{
    std::string needle = normalize_token(name);

    if(needle.empty()) {
        return nullptr;
    }
    for(size_t i = 0; i < ARRAY_SIZE(meshtastic_regions); i++) {
        if(normalize_token(meshtastic_regions[i].name) == needle) {
            return &meshtastic_regions[i];
        }
    }
    return nullptr;
}

static bool profile_supports_preset(region_profile_type_t profile,
                                    const char *preset_name)
{
    std::string name = normalize_token(preset_name);

    switch(profile) {
    case REGION_PROFILE_STD:
        return name == "LONGFAST" || name == "LONGSLOW" ||
               name == "MEDIUMSLOW" || name == "MEDIUMFAST" ||
               name == "SHORTSLOW" || name == "SHORTFAST" ||
               name == "LONGMODERATE" || name == "SHORTTURBO" ||
               name == "LONGTURBO" || name == "MEDIUMTURBO";
    case REGION_PROFILE_EU868:
        return name == "LONGFAST" || name == "LONGSLOW" ||
               name == "MEDIUMSLOW" || name == "MEDIUMFAST" ||
               name == "SHORTSLOW" || name == "SHORTFAST" ||
               name == "LONGMODERATE";
    case REGION_PROFILE_LITE:
        return name == "LITEFAST" || name == "LITESLOW";
    case REGION_PROFILE_NARROW:
    case REGION_PROFILE_HAM_100KHZ:
        return name == "NARROWFAST" || name == "NARROWSLOW";
    case REGION_PROFILE_HAM_20KHZ:
        return name == "TINYFAST" || name == "TINYSLOW";
    default:
        return false;
    }
}

static std::string effective_mesh_channel_name(const probe_options_t &opts)
{
    const meshtastic_preset_t *preset;

    if(!opts.channel_name.empty()) {
        return opts.channel_name;
    }
    preset = find_meshtastic_preset(
        opts.resolved_preset.empty() ? MESHTASTIC_DEFAULT_PRESET :
                                      opts.resolved_preset.c_str());
    return preset ? preset->display_name : "LongFast";
}

static bool apply_meshtastic_profile(probe_options_t *opts)
{
    probe_profile_t parsed_profile;
    const meshtastic_region_t *region;
    const meshtastic_preset_t *preset;
    const meshtastic_region_profile_t *region_profile;
    const char *region_name;
    const char *preset_name;
    std::string frequency_hash_name;
    float bandwidth;
    float slot_width;
    float span;
    uint32_t num_slots;
    uint32_t channel_hash_slot;
    uint32_t preset_hash_slot;
    uint32_t slot0;
    int power;

    if(!opts) {
        return false;
    }
    if(!opts->mesh_mode && opts->region.empty() && opts->preset.empty() &&
       opts->frequency_slot == 0U) {
        return true;
    }

    parsed_profile = opts->profile;
    region_name = opts->region.empty() ? MESHTASTIC_DEFAULT_REGION :
                                         opts->region.c_str();
    region = find_meshtastic_region(region_name);
    if(!region) {
        fprintf(stderr, "Unknown Meshtastic region: %s\n", region_name);
        return false;
    }

    preset_name = opts->preset.empty() ? region->default_preset :
                                         opts->preset.c_str();
    preset = find_meshtastic_preset(preset_name);
    if(!preset) {
        fprintf(stderr, "Unknown Meshtastic preset: %s\n", preset_name);
        return false;
    }
    if(!profile_supports_preset(region->profile, preset->name)) {
        fprintf(stderr, "Preset %s is invalid for region %s\n",
                preset->name, region->name);
        return false;
    }

    if((size_t)region->profile >= ARRAY_SIZE(region_profiles)) {
        fprintf(stderr, "Internal region profile error for %s\n", region->name);
        return false;
    }
    region_profile = &region_profiles[(size_t)region->profile];
    bandwidth = opts->manual_bw ? parsed_profile.bandwidth :
                                  (region->wide_lora ? preset->wide_bandwidth :
                                                       preset->bandwidth);
    span = region->freq_end - region->freq_start;
    slot_width = region_profile->spacing + (region_profile->padding * 2.0f) +
                 (bandwidth / 1000.0f);
    if(bandwidth <= 0.0f || slot_width <= 0.0f || span < slot_width) {
        fprintf(stderr,
                "Region %s span %.0fkHz is too small for %.1fkHz bandwidth\n",
                region->name, span * 1000.0f, bandwidth);
        return false;
    }

    num_slots = (uint32_t)roundf((span + region_profile->spacing) / slot_width);
    if(num_slots == 0U) {
        fprintf(stderr, "Region %s produced zero frequency slots\n",
                region->name);
        return false;
    }

    frequency_hash_name = opts->channel_name.empty() ? preset->display_name :
                                                opts->channel_name.c_str();
    channel_hash_slot = djb2_hash(frequency_hash_name.c_str()) % num_slots;
    preset_hash_slot = djb2_hash(preset->display_name) % num_slots;

    if(opts->frequency_slot != 0U) {
        if(opts->frequency_slot > num_slots) {
            fprintf(stderr, "Frequency slot %u invalid for %s, max is %u\n",
                    opts->frequency_slot, region->name, num_slots);
            return false;
        }
        slot0 = opts->frequency_slot - 1U;
    } else if(region->override_slot > 0) {
        slot0 = (uint32_t)region->override_slot - 1U;
    } else if(region->override_slot == OVERRIDE_SLOT_PRESET_HASH) {
        slot0 = preset_hash_slot;
    } else {
        slot0 = channel_hash_slot;
    }

    power = region->power_limit;
    if(power <= 0) {
        power = 17;
    }
    if(power > MESHTASTIC_MAX_K230_TX_POWER_DBM) {
        power = MESHTASTIC_MAX_K230_TX_POWER_DBM;
    }

    opts->profile.freq = region->freq_start + (bandwidth / 2000.0f) +
                         region_profile->padding + ((float)slot0 * slot_width);
    opts->profile.bandwidth = bandwidth;
    opts->profile.sf = preset->sf;
    opts->profile.cr = preset->cr;
    opts->profile.sync_word = MESHTASTIC_SYNC_WORD;
    opts->profile.power = (int8_t)power;
    opts->profile.preamble = region->wide_lora ? 12U : 16U;

    if(opts->manual_freq) {
        opts->profile.freq = parsed_profile.freq;
    }
    if(opts->manual_sf) {
        opts->profile.sf = parsed_profile.sf;
    }
    if(opts->manual_cr) {
        opts->profile.cr = parsed_profile.cr;
    }
    if(opts->manual_sw) {
        opts->profile.sync_word = parsed_profile.sync_word;
    }
    if(opts->manual_power) {
        opts->profile.power = parsed_profile.power;
    }
    if(opts->manual_preamble) {
        opts->profile.preamble = parsed_profile.preamble;
    }

    opts->resolved_region = region->name;
    opts->resolved_preset = preset->name;
    opts->resolved_slot = slot0 + 1U;
    opts->resolved_slot_count = num_slots;
    return true;
}

static uint8_t xor_hash(const uint8_t *data, size_t len)
{
    uint8_t h = 0;
    for(size_t i = 0; i < len; i++) {
        h ^= data[i];
    }
    return h;
}

static bool parse_hex_nibble(char c, uint8_t *out)
{
    if(c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if(c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    if(c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

static bool parse_psk(const std::string &text, std::vector<uint8_t> *key)
{
    std::string hex;

    if(!key) {
        return false;
    }
    key->clear();
    if(text == "none" || text == "off" || text == "0") {
        return true;
    }
    if(text == "default" || text.empty()) {
        key->assign(default_psk, default_psk + sizeof(default_psk));
        return true;
    }

    hex = text;
    if(hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
        hex.erase(0, 2);
    }
    if(hex.size() % 2U != 0U || hex.empty() || hex.size() > 64U) {
        return false;
    }
    for(size_t i = 0; i < hex.size(); i += 2U) {
        uint8_t hi;
        uint8_t lo;
        if(!parse_hex_nibble(hex[i], &hi) ||
           !parse_hex_nibble(hex[i + 1U], &lo)) {
            return false;
        }
        key->push_back((uint8_t)((hi << 4) | lo));
    }
    return key->size() == 16U || key->size() == 32U;
}

static uint8_t mesh_channel_hash(const std::string &name,
                                 const std::vector<uint8_t> &key)
{
    uint8_t h = xor_hash((const uint8_t *)name.data(), name.size());
    if(!key.empty()) {
        h ^= xor_hash(key.data(), key.size());
    }
    return h;
}

static void append_varint(std::vector<uint8_t> *out, uint32_t value)
{
    while(value >= 0x80U) {
        out->push_back((uint8_t)((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    out->push_back((uint8_t)value);
}

static void append_fixed32(std::vector<uint8_t> *out, uint32_t value)
{
    size_t start = out->size();

    out->resize(start + 4U);
    put_le32(out->data() + start, value);
}

static bool read_varint(const uint8_t *data, size_t len, size_t *pos,
                        uint32_t *value)
{
    uint32_t result = 0;
    uint32_t shift = 0;

    if(!data || !pos || !value) {
        return false;
    }
    while(*pos < len && shift < 32U) {
        uint8_t b = data[(*pos)++];
        result |= (uint32_t)(b & 0x7fU) << shift;
        if((b & 0x80U) == 0U) {
            *value = result;
            return true;
        }
        shift += 7U;
    }
    return false;
}

static bool read_varint64(const uint8_t *data, size_t len, size_t *pos,
                          uint64_t *value)
{
    uint64_t result = 0;
    uint32_t shift = 0;

    if(!data || !pos || !value) {
        return false;
    }
    while(*pos < len && shift < 64U) {
        uint8_t b = data[(*pos)++];
        result |= (uint64_t)(b & 0x7fU) << shift;
        if((b & 0x80U) == 0U) {
            *value = result;
            return true;
        }
        shift += 7U;
    }
    return false;
}

static float fixed32_to_float(uint32_t value)
{
    float out;

    memcpy(&out, &value, sizeof(out));
    return out;
}

static bool encode_data_proto(uint32_t portnum,
                              const std::vector<uint8_t> &payload,
                              uint32_t request_id,
                              uint32_t reply_id,
                              std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    out->push_back(0x08U);
    append_varint(out, portnum);
    if(!payload.empty()) {
        out->push_back(0x12U);
        append_varint(out, (uint32_t)payload.size());
        out->insert(out->end(), payload.begin(), payload.end());
    }
    if(request_id != 0U) {
        out->push_back(0x35U);
        append_fixed32(out, request_id);
    }
    if(reply_id != 0U) {
        out->push_back(0x3dU);
        append_fixed32(out, reply_id);
    }
    return out->size() <= (MESHTASTIC_MAX_LORA_PAYLOAD_LEN -
                           MESHTASTIC_HEADER_LENGTH);
}

static bool encode_routing_proto(uint32_t error_reason,
                                 std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    out->push_back(0x18U);
    append_varint(out, error_reason);
    return true;
}

static void append_string_field(std::vector<uint8_t> *out, uint32_t field,
                                const std::string &value, size_t max_len)
{
    size_t len;

    if(!out || value.empty()) {
        return;
    }
    len = value.size();
    if(len > max_len) {
        len = max_len;
    }
    append_varint(out, (field << 3U) | 2U);
    append_varint(out, (uint32_t)len);
    out->insert(out->end(), value.begin(), value.begin() + len);
}

static std::string make_short_node_name(const std::string &name)
{
    std::string out;

    for(char c : name) {
        unsigned char uc = (unsigned char)c;
        if(isalnum(uc)) {
            out.push_back((char)toupper(uc));
            if(out.size() >= 4U) {
                break;
            }
        }
    }
    if(out.empty()) {
        out = "K230";
    }
    return out;
}

static bool encode_user_proto(const probe_options_t &opts,
                              std::vector<uint8_t> *out)
{
    char id[16];
    std::string long_name = mesh_clean_text(opts.node_name);
    std::string short_name;

    if(!out) {
        return false;
    }
    if(long_name.empty() || long_name == "k230-t-display") {
        long_name = phoneapi_default_node_name(opts);
    }
    short_name = make_short_node_name(long_name);

    out->clear();
    snprintf(id, sizeof(id), "!%08x", opts.from_node);
    append_string_field(out, 1U, id, 15U);
    append_string_field(out, 2U, long_name, 39U);
    append_string_field(out, 3U, short_name, 4U);
    append_varint(out, (5U << 3U) | 0U);
    append_varint(out, 0U);
    return !out->empty();
}

static void append_bytes_field(std::vector<uint8_t> *out, uint32_t field,
                               const uint8_t *data, size_t len)
{
    if(!out || (!data && len > 0U)) {
        return;
    }
    append_varint(out, (field << 3U) | 2U);
    append_varint(out, (uint32_t)len);
    if(len > 0U) {
        out->insert(out->end(), data, data + len);
    }
}

static void append_bytes_field(std::vector<uint8_t> *out, uint32_t field,
                               const std::vector<uint8_t> &value)
{
    append_bytes_field(out, field, value.empty() ? nullptr : value.data(),
                       value.size());
}

static void append_uint32_field(std::vector<uint8_t> *out, uint32_t field,
                                uint32_t value)
{
    if(!out) {
        return;
    }
    append_varint(out, (field << 3U) | 0U);
    append_varint(out, value);
}

static void append_bool_field(std::vector<uint8_t> *out, uint32_t field,
                              bool value)
{
    append_uint32_field(out, field, value ? 1U : 0U);
}

static void append_float_field(std::vector<uint8_t> *out, uint32_t field,
                               float value)
{
    uint32_t raw;

    if(!out) {
        return;
    }
    memcpy(&raw, &value, sizeof(raw));
    append_varint(out, (field << 3U) | 5U);
    append_fixed32(out, raw);
}

static uint32_t phoneapi_region_enum(const std::string &name)
{
    struct region_map_t {
        const char *name;
        uint32_t value;
    };
    static const region_map_t map[] = {
        {"UNSET", 0}, {"US", 1}, {"EU_433", 2}, {"EU_868", 3},
        {"CN", 4}, {"JP", 5}, {"ANZ", 6}, {"KR", 7}, {"TW", 8},
        {"RU", 9}, {"IN", 10}, {"NZ_865", 11}, {"TH", 12},
        {"LORA_24", 13}, {"UA_433", 14}, {"UA_868", 15},
        {"MY_433", 16}, {"MY_919", 17}, {"SG_923", 18},
        {"PH_433", 19}, {"PH_868", 20}, {"PH_915", 21},
        {"ANZ_433", 22}, {"KZ_433", 23}, {"KZ_863", 24},
        {"NP_865", 25}, {"BR_902", 26}, {"ITU1_2M", 27},
        {"ITU2_2M", 28}, {"EU_866", 29}, {"EU_874", 30},
        {"EU_917", 31}, {"EU_N_868", 32}, {"ITU3_2M", 33},
        {"ITU1_70CM", 34}, {"ITU2_70CM", 35}, {"ITU3_70CM", 36},
        {"ITU2_125CM", 37},
    };
    std::string needle = normalize_token(name.c_str());

    for(size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if(normalize_token(map[i].name) == needle) {
            return map[i].value;
        }
    }
    return 1U;
}

static uint32_t phoneapi_preset_enum(const std::string &name)
{
    struct preset_map_t {
        const char *name;
        uint32_t value;
    };
    static const preset_map_t map[] = {
        {"LONG_FAST", 0}, {"LONG_SLOW", 1}, {"VERY_LONG_SLOW", 2},
        {"MEDIUM_SLOW", 3}, {"MEDIUM_FAST", 4}, {"SHORT_SLOW", 5},
        {"SHORT_FAST", 6}, {"LONG_MODERATE", 7}, {"SHORT_TURBO", 8},
        {"LONG_TURBO", 9}, {"LITE_FAST", 10}, {"LITE_SLOW", 11},
        {"NARROW_FAST", 12}, {"NARROW_SLOW", 13}, {"TINY_FAST", 14},
        {"TINY_SLOW", 15}, {"MEDIUM_TURBO", 16},
    };
    std::string needle = normalize_token(name.c_str());

    for(size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if(normalize_token(map[i].name) == needle) {
            return map[i].value;
        }
    }
    return 0U;
}

static bool encode_phoneapi_user_proto(const probe_options_t &opts,
                                       std::vector<uint8_t> *out)
{
    char id[16];
    uint8_t mac[6];
    std::string long_name = mesh_clean_text(opts.node_name);
    std::string short_name;

    if(!out) {
        return false;
    }
    if(long_name.empty() || long_name == "k230-t-display") {
        long_name = phoneapi_default_node_name(opts);
    }
    short_name = make_short_node_name(long_name);
    out->clear();
    snprintf(id, sizeof(id), "!%08x", opts.from_node);
    mac[0] = 0x52U;
    mac[1] = 0x40U;
    mac[2] = (uint8_t)(opts.from_node >> 24);
    mac[3] = (uint8_t)(opts.from_node >> 16);
    mac[4] = (uint8_t)(opts.from_node >> 8);
    mac[5] = (uint8_t)opts.from_node;
    append_string_field(out, 1U, id, 15U);
    append_string_field(out, 2U, long_name, 39U);
    append_string_field(out, 3U, short_name, 4U);
    append_bytes_field(out, 4U, mac, sizeof(mac));
    append_uint32_field(out, 5U, MESHTASTIC_HW_MODEL_NRF52840_PCA10059);
    return true;
}

static bool encode_phoneapi_my_node_info(const probe_options_t &opts,
                                         std::vector<uint8_t> *out)
{
    uint8_t device_id[8];

    if(!out) {
        return false;
    }
    out->clear();
    put_le32(device_id, opts.from_node);
    put_le32(device_id + 4, djb2_hash(opts.node_name.c_str()));
    append_uint32_field(out, 1U, opts.from_node);
    append_uint32_field(out, 8U, 1U);
    append_bytes_field(out, 12U, device_id, sizeof(device_id));
    append_string_field(out, 13U, "nrf52840_pca10059", 31U);
    append_uint32_field(out, 15U, 1U);
    return true;
}

static bool encode_phoneapi_metadata(std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    append_string_field(out, 1U, "k230-nrf52840-phoneapi-0.1", 63U);
    append_uint32_field(out, 2U, 1U);
    append_bool_field(out, 3U, true);
    append_bool_field(out, 4U, false);
    append_bool_field(out, 5U, true);
    append_bool_field(out, 6U, true);
    append_uint32_field(out, 9U, MESHTASTIC_HW_MODEL_NRF52840_PCA10059);
    return true;
}

static void append_phoneapi_region_preset_group(
    std::vector<uint8_t> *out, const uint32_t *presets, size_t preset_count,
    uint32_t default_preset, bool licensed_only)
{
    std::vector<uint8_t> group;

    if(!out || !presets || preset_count == 0U) {
        return;
    }
    for(size_t i = 0; i < preset_count; i++) {
        append_uint32_field(&group, 1U, presets[i]);
    }
    append_uint32_field(&group, 2U, default_preset);
    append_bool_field(&group, 3U, licensed_only);
    append_bytes_field(out, 1U, group);
}

static void append_phoneapi_region_group(std::vector<uint8_t> *out,
                                         uint32_t region,
                                         uint32_t group_index)
{
    std::vector<uint8_t> entry;

    if(!out) {
        return;
    }
    append_uint32_field(&entry, 1U, region);
    append_uint32_field(&entry, 2U, group_index);
    append_bytes_field(out, 2U, entry);
}

static bool encode_phoneapi_region_presets(std::vector<uint8_t> *out)
{
    enum {
        REGION_US = 1U,
        REGION_EU_433 = 2U,
        REGION_EU_868 = 3U,
        REGION_CN = 4U,
        REGION_JP = 5U,
        REGION_ANZ = 6U,
        REGION_KR = 7U,
        REGION_TW = 8U,
        REGION_RU = 9U,
        REGION_IN = 10U,
        REGION_NZ_865 = 11U,
        REGION_TH = 12U,
        REGION_LORA_24 = 13U,
        REGION_UA_433 = 14U,
        REGION_MY_433 = 16U,
        REGION_MY_919 = 17U,
        REGION_SG_923 = 18U,
        REGION_PH_433 = 19U,
        REGION_PH_868 = 20U,
        REGION_PH_915 = 21U,
        REGION_ANZ_433 = 22U,
        REGION_KZ_433 = 23U,
        REGION_KZ_863 = 24U,
        REGION_NP_865 = 25U,
        REGION_BR_902 = 26U,
        REGION_ITU1_2M = 27U,
        REGION_ITU2_2M = 28U,
        REGION_EU_866 = 29U,
        REGION_EU_N_868 = 32U,
        REGION_ITU3_2M = 33U,
        REGION_ITU1_70CM = 34U,
        REGION_ITU2_70CM = 35U,
        REGION_ITU3_70CM = 36U,
        REGION_ITU2_125CM = 37U,
    };
    enum {
        GROUP_STD = 0U,
        GROUP_EU868 = 1U,
        GROUP_LITE = 2U,
        GROUP_EU_NARROW = 3U,
        GROUP_HAM_20KHZ = 4U,
        GROUP_HAM_100KHZ = 5U,
    };
    static const uint32_t presets_std[] = {0U, 1U, 3U, 4U, 5U,
                                           6U, 7U, 8U, 9U, 16U};
    static const uint32_t presets_narrow[] = {12U, 13U};
    static const uint32_t presets_tiny[] = {14U, 15U};
    static const uint32_t presets_eu_superset[] = {
        0U, 1U, 3U, 4U, 5U, 6U, 7U, 10U, 11U, 12U, 13U};
    static const uint32_t std_regions[] = {
        REGION_US,      REGION_EU_433, REGION_CN,     REGION_JP,
        REGION_ANZ,     REGION_KR,     REGION_TW,     REGION_RU,
        REGION_IN,      REGION_NZ_865, REGION_TH,     REGION_LORA_24,
        REGION_UA_433,  REGION_MY_433, REGION_MY_919, REGION_SG_923,
        REGION_PH_433,  REGION_PH_868, REGION_PH_915, REGION_ANZ_433,
        REGION_KZ_433,  REGION_KZ_863, REGION_NP_865, REGION_BR_902,
    };
    static const uint32_t ham20_regions[] = {
        REGION_ITU1_2M, REGION_ITU2_2M, REGION_ITU3_2M};
    static const uint32_t ham100_regions[] = {
        REGION_ITU2_125CM, REGION_ITU1_70CM, REGION_ITU2_70CM,
        REGION_ITU3_70CM};

    if(!out) {
        return false;
    }
    out->clear();

    append_phoneapi_region_preset_group(out, presets_std,
                                        ARRAY_SIZE(presets_std), 0U, false);
    append_phoneapi_region_preset_group(out, presets_eu_superset,
                                        ARRAY_SIZE(presets_eu_superset), 0U,
                                        false);
    append_phoneapi_region_preset_group(out, presets_eu_superset,
                                        ARRAY_SIZE(presets_eu_superset), 10U,
                                        false);
    append_phoneapi_region_preset_group(out, presets_eu_superset,
                                        ARRAY_SIZE(presets_eu_superset), 13U,
                                        false);
    append_phoneapi_region_preset_group(out, presets_tiny,
                                        ARRAY_SIZE(presets_tiny), 14U, true);
    append_phoneapi_region_preset_group(out, presets_narrow,
                                        ARRAY_SIZE(presets_narrow), 13U, true);

    for(size_t i = 0; i < ARRAY_SIZE(std_regions); i++) {
        append_phoneapi_region_group(out, std_regions[i], GROUP_STD);
    }
    append_phoneapi_region_group(out, REGION_EU_868, GROUP_EU868);
    append_phoneapi_region_group(out, REGION_EU_866, GROUP_LITE);
    append_phoneapi_region_group(out, REGION_EU_N_868, GROUP_EU_NARROW);
    for(size_t i = 0; i < ARRAY_SIZE(ham20_regions); i++) {
        append_phoneapi_region_group(out, ham20_regions[i], GROUP_HAM_20KHZ);
    }
    for(size_t i = 0; i < ARRAY_SIZE(ham100_regions); i++) {
        append_phoneapi_region_group(out, ham100_regions[i], GROUP_HAM_100KHZ);
    }
    return !out->empty();
}

static bool encode_phoneapi_config_device(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> device;

    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(&device, 1U, 0U);
    append_bytes_field(out, 1U, device);
    return true;
}

static bool encode_phoneapi_config_lora(const probe_options_t &opts,
                                        std::vector<uint8_t> *out)
{
    std::vector<uint8_t> lora;
    uint32_t region = phoneapi_region_enum(
        opts.resolved_region.empty() ? opts.region : opts.resolved_region);
    uint32_t preset = phoneapi_preset_enum(
        opts.resolved_preset.empty() ? opts.preset : opts.resolved_preset);

    if(!out) {
        return false;
    }
    out->clear();
    append_bool_field(&lora, 1U, true);
    append_uint32_field(&lora, 2U, preset);
    append_uint32_field(&lora, 7U, region);
    append_uint32_field(&lora, 8U, opts.hop_limit);
    append_bool_field(&lora, 9U, true);
    if(opts.profile.power > 0) {
        append_uint32_field(&lora, 10U, (uint32_t)opts.profile.power);
    }
    if(opts.resolved_slot > 0U) {
        append_uint32_field(&lora, 11U, opts.resolved_slot);
    }
    append_bytes_field(out, 6U, lora);
    return true;
}

static bool encode_phoneapi_config_bluetooth(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> bluetooth;

    if(!out) {
        return false;
    }
    out->clear();
    append_bool_field(&bluetooth, 1U, true);
    append_uint32_field(&bluetooth, 2U, 2U);
    append_bytes_field(out, 7U, bluetooth);
    return true;
}

static bool encode_phoneapi_channel(const probe_options_t &opts,
                                    std::vector<uint8_t> *out)
{
    std::vector<uint8_t> channel;
    std::vector<uint8_t> settings;
    std::vector<uint8_t> key;
    std::string channel_name = effective_mesh_channel_name(opts);

    if(!out) {
        return false;
    }
    out->clear();
    (void)parse_psk(opts.psk, &key);
    if(!key.empty()) {
        append_bytes_field(&settings, 2U, key);
    }
    append_string_field(&settings, 3U, channel_name, 31U);
    append_uint32_field(&channel, 1U, 0U);
    append_bytes_field(&channel, 2U, settings);
    append_uint32_field(&channel, 3U, 1U);
    *out = channel;
    return true;
}

static bool encode_phoneapi_node_info(const probe_options_t &opts,
                                      std::vector<uint8_t> *out)
{
    std::vector<uint8_t> user;
    uint32_t now = (uint32_t)time(nullptr);

    if(!out || !encode_phoneapi_user_proto(opts, &user)) {
        return false;
    }
    out->clear();
    append_uint32_field(out, 1U, opts.from_node);
    append_bytes_field(out, 2U, user);
    append_varint(out, (5U << 3U) | 5U);
    append_fixed32(out, now);
    append_uint32_field(out, 9U, 0U);
    return true;
}

static uint32_t phoneapi_next_from_id(void)
{
    static uint32_t id = 1U;
    return __sync_fetch_and_add(&id, 1U);
}

static bool encode_phoneapi_from_payload(uint32_t field,
                                         const std::vector<uint8_t> &payload,
                                         std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(out, 1U, phoneapi_next_from_id());
    append_bytes_field(out, field, payload);
    return true;
}

static bool encode_phoneapi_config_complete(uint32_t nonce,
                                            std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(out, 1U, phoneapi_next_from_id());
    append_uint32_field(out, 7U, nonce);
    return true;
}

static std::string phoneapi_hex_encode(const std::vector<uint8_t> &data)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;

    out.reserve(data.size() * 2U);
    for(uint8_t b : data) {
        out.push_back(hex[(b >> 4U) & 0x0fU]);
        out.push_back(hex[b & 0x0fU]);
    }
    return out;
}

static bool phoneapi_hex_decode(const char *hex, size_t len,
                                std::vector<uint8_t> *out)
{
    if(!hex || !out || (len % 2U) != 0U) {
        return false;
    }
    out->clear();
    out->reserve(len / 2U);
    for(size_t i = 0; i < len; i += 2U) {
        uint8_t hi;
        uint8_t lo;
        if(!parse_hex_nibble(hex[i], &hi) ||
           !parse_hex_nibble(hex[i + 1U], &lo)) {
            return false;
        }
        out->push_back((uint8_t)((hi << 4U) | lo));
    }
    return true;
}

static uint8_t mesh_header_hop_limit(const mesh_header_t &header);
static uint8_t mesh_header_hop_start(const mesh_header_t &header);
static bool mesh_header_want_ack(const mesh_header_t &header);
static bool decode_data_proto(const uint8_t *data, size_t len,
                              mesh_data_proto_t *decoded);
static bool phoneapi_send_from_payload(int fd, uint32_t field,
                                       const std::vector<uint8_t> &payload,
                                       const char *label);

typedef struct {
    bool has_want_config = false;
    uint32_t want_config_id = 0;
    bool disconnect = false;
    bool heartbeat = false;
    size_t packet_len = 0;
    std::vector<uint8_t> packet;
} phoneapi_to_radio_t;

typedef struct {
    bool get_owner_request = false;
    bool has_get_channel_request = false;
    uint32_t get_channel_request = 0;
    bool has_get_config_request = false;
    uint32_t get_config_request = 0;
    bool has_get_module_config_request = false;
    uint32_t get_module_config_request = 0;
    bool get_device_metadata_request = false;
    bool has_set_time_only = false;
    uint32_t set_time_only = 0;
    bool has_set_owner = false;
    std::vector<uint8_t> set_owner;
    bool has_set_channel = false;
    std::vector<uint8_t> set_channel;
    bool has_set_config = false;
    std::vector<uint8_t> set_config;
    bool has_set_module_config = false;
    std::vector<uint8_t> set_module_config;
} phoneapi_admin_request_t;

static bool phoneapi_proto_skip(const uint8_t *data, size_t len, size_t *pos,
                                uint32_t wire)
{
    uint32_t l;
    uint64_t ignored;

    switch(wire) {
    case 0U:
        return read_varint64(data, len, pos, &ignored);
    case 1U:
        if(*pos + 8U > len) {
            return false;
        }
        *pos += 8U;
        return true;
    case 2U:
        if(!read_varint(data, len, pos, &l) || *pos + l > len) {
            return false;
        }
        *pos += l;
        return true;
    case 5U:
        if(*pos + 4U > len) {
            return false;
        }
        *pos += 4U;
        return true;
    default:
        return false;
    }
}

static bool phoneapi_parse_to_radio(const uint8_t *data, size_t len,
                                    phoneapi_to_radio_t *out)
{
    size_t pos = 0;

    if(!data || !out) {
        return false;
    }
    *out = phoneapi_to_radio_t();
    while(pos < len) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(data, len, &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 1U && wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            out->packet_len = l;
            out->packet.assign(data + pos, data + pos + l);
            pos += l;
        } else if(field == 3U && wire == 0U) {
            if(!read_varint(data, len, &pos, &out->want_config_id)) {
                return false;
            }
            out->has_want_config = true;
        } else if(field == 4U && wire == 0U) {
            uint32_t value;
            if(!read_varint(data, len, &pos, &value)) {
                return false;
            }
            out->disconnect = value != 0U;
        } else if(field == 7U && wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            out->heartbeat = true;
            pos += l;
        } else if(!phoneapi_proto_skip(data, len, &pos, wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_admin_request(const std::vector<uint8_t> &payload,
                                         phoneapi_admin_request_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    *out = phoneapi_admin_request_t();
    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 1U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->get_channel_request)) {
                return false;
            }
            out->has_get_channel_request = true;
        } else if(field == 3U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->get_owner_request = value != 0U;
        } else if(field == 5U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->get_config_request)) {
                return false;
            }
            out->has_get_config_request = true;
        } else if(field == 7U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->get_module_config_request)) {
                return false;
            }
            out->has_get_module_config_request = true;
        } else if(field == 12U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->get_device_metadata_request = value != 0U;
        } else if(field == 43U && wire == 5U &&
                  pos + 4U <= payload.size()) {
            out->set_time_only = get_le32(payload.data() + pos);
            out->has_set_time_only = true;
            pos += 4U;
        } else if((field == 32U || field == 33U || field == 34U ||
                   field == 35U) && wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            if(field == 32U) {
                out->set_owner.assign(payload.begin() + (long)pos,
                                      payload.begin() + (long)(pos + l));
                out->has_set_owner = true;
            } else if(field == 33U) {
                out->set_channel.assign(payload.begin() + (long)pos,
                                        payload.begin() + (long)(pos + l));
                out->has_set_channel = true;
            } else if(field == 34U) {
                out->set_config.assign(payload.begin() + (long)pos,
                                       payload.begin() + (long)(pos + l));
                out->has_set_config = true;
            } else {
                out->set_module_config.assign(
                    payload.begin() + (long)pos,
                    payload.begin() + (long)(pos + l));
                out->has_set_module_config = true;
            }
            pos += l;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_mesh_packet(const std::vector<uint8_t> &packet,
                                       phoneapi_mesh_tx_t *out)
{
    size_t pos = 0;
    phoneapi_mesh_tx_t found;
    bool has_decoded = false;

    if(!out) {
        return false;
    }
    while(pos < packet.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(packet.data(), packet.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 1U && wire == 5U && pos + 4U <= packet.size()) {
            found.from_node = get_le32(packet.data() + pos);
            pos += 4U;
        } else if(field == 2U && wire == 5U && pos + 4U <= packet.size()) {
            found.to_node = get_le32(packet.data() + pos);
            pos += 4U;
        } else if(field == 4U && wire == 2U) {
            uint32_t l;
            if(!read_varint(packet.data(), packet.size(), &pos, &l) ||
               pos + l > packet.size()) {
                return false;
            }
            has_decoded = decode_data_proto(packet.data() + pos, l,
                                            &found.data);
            pos += l;
        } else if(field == 6U && wire == 5U && pos + 4U <= packet.size()) {
            found.packet_id = get_le32(packet.data() + pos);
            pos += 4U;
        } else if(field == 9U && wire == 0U) {
            uint32_t hop;
            if(!read_varint(packet.data(), packet.size(), &pos, &hop)) {
                return false;
            }
            found.hop_limit = (uint8_t)(hop & 0x07U);
        } else if(field == 10U && wire == 0U) {
            uint32_t value;
            if(!read_varint(packet.data(), packet.size(), &pos, &value)) {
                return false;
            }
            found.want_ack = value != 0U;
        } else if(!phoneapi_proto_skip(packet.data(), packet.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    if(!has_decoded || found.data.portnum == 0U) {
        return false;
    }
    if(found.hop_limit == 0U) {
        found.hop_limit = phoneapi_opts.hop_limit;
    }
    found.active = true;
    *out = found;
    return true;
}

typedef struct {
    bool has_name = false;
    std::string name;
} phoneapi_owner_update_t;

typedef struct {
    bool has_name = false;
    bool has_psk = false;
    std::string name;
    std::string psk;
} phoneapi_channel_update_t;

typedef struct {
    bool has_lora = false;
    bool has_region = false;
    bool has_preset = false;
    bool has_hop_limit = false;
    bool has_tx_power = false;
    std::string region;
    std::string preset;
    uint32_t hop_limit = 0;
    int32_t tx_power = 0;
} phoneapi_config_update_t;

static bool phoneapi_read_length_delimited(const std::vector<uint8_t> &payload,
                                           size_t *pos,
                                           std::vector<uint8_t> *out)
{
    uint32_t l;

    if(!pos || !out ||
       !read_varint(payload.data(), payload.size(), pos, &l) ||
       *pos + l > payload.size()) {
        return false;
    }
    out->assign(payload.begin() + (long)*pos,
                payload.begin() + (long)(*pos + l));
    *pos += l;
    return true;
}

static bool phoneapi_read_string_field(const std::vector<uint8_t> &payload,
                                       size_t *pos, std::string *out)
{
    std::vector<uint8_t> bytes;

    if(!out || !phoneapi_read_length_delimited(payload, pos, &bytes)) {
        return false;
    }
    out->assign((const char *)bytes.data(), bytes.size());
    return true;
}

static bool phoneapi_parse_user_update(const std::vector<uint8_t> &payload,
                                       phoneapi_owner_update_t *out)
{
    size_t pos = 0;
    std::string short_name;

    if(!out) {
        return false;
    }
    *out = phoneapi_owner_update_t();
    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 2U && wire == 2U) {
            if(!phoneapi_read_string_field(payload, &pos, &out->name)) {
                return false;
            }
            out->has_name = !mesh_clean_text(out->name).empty();
        } else if(field == 3U && wire == 2U) {
            if(!phoneapi_read_string_field(payload, &pos, &short_name)) {
                return false;
            }
            if(!out->has_name && !mesh_clean_text(short_name).empty()) {
                out->name = short_name;
                out->has_name = true;
            }
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static std::string phoneapi_psk_bytes_to_text(const std::vector<uint8_t> &psk)
{
    if(psk.empty()) {
        return "none";
    }
    if(psk.size() == sizeof(default_psk) &&
       memcmp(psk.data(), default_psk, sizeof(default_psk)) == 0) {
        return "default";
    }
    return phoneapi_hex_encode(psk);
}

static bool phoneapi_parse_channel_settings(
    const std::vector<uint8_t> &payload, phoneapi_channel_update_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 2U && wire == 2U) {
            std::vector<uint8_t> key;
            if(!phoneapi_read_length_delimited(payload, &pos, &key)) {
                return false;
            }
            out->psk = phoneapi_psk_bytes_to_text(key);
            out->has_psk = true;
        } else if(field == 3U && wire == 2U) {
            if(!phoneapi_read_string_field(payload, &pos, &out->name)) {
                return false;
            }
            out->name = mesh_clean_text(out->name);
            out->has_name = true;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_channel_update(const std::vector<uint8_t> &payload,
                                          phoneapi_channel_update_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    *out = phoneapi_channel_update_t();
    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 2U && wire == 2U) {
            std::vector<uint8_t> settings;
            if(!phoneapi_read_length_delimited(payload, &pos, &settings) ||
               !phoneapi_parse_channel_settings(settings, out)) {
                return false;
            }
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static const char *phoneapi_region_name_from_enum(uint32_t value)
{
    struct map_t {
        uint32_t value;
        const char *name;
    };
    static const map_t map[] = {
        {0, "UNSET"}, {1, "US"}, {2, "EU_433"}, {3, "EU_868"},
        {4, "CN"}, {5, "JP"}, {6, "ANZ"}, {7, "KR"},
        {8, "TW"}, {9, "RU"}, {10, "IN"}, {11, "NZ_865"},
        {12, "TH"}, {13, "LORA_24"}, {14, "UA_433"},
        {16, "MY_433"}, {17, "MY_919"}, {18, "SG_923"},
        {19, "PH_433"}, {20, "PH_868"}, {21, "PH_915"},
        {22, "ANZ_433"}, {23, "KZ_433"}, {24, "KZ_863"},
        {25, "NP_865"}, {26, "BR_902"}, {27, "ITU1_2M"},
        {28, "ITU2_2M"}, {29, "EU_866"}, {32, "EU_N_868"},
        {33, "ITU3_2M"}, {34, "ITU1_70CM"}, {35, "ITU2_70CM"},
        {36, "ITU3_70CM"}, {37, "ITU2_125CM"},
    };

    for(size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if(map[i].value == value) {
            return map[i].name;
        }
    }
    return nullptr;
}

static const char *phoneapi_preset_name_from_enum(uint32_t value)
{
    struct map_t {
        uint32_t value;
        const char *name;
    };
    static const map_t map[] = {
        {0, "LONG_FAST"}, {1, "LONG_SLOW"}, {3, "MEDIUM_SLOW"},
        {4, "MEDIUM_FAST"}, {5, "SHORT_SLOW"}, {6, "SHORT_FAST"},
        {7, "LONG_MODERATE"}, {8, "SHORT_TURBO"}, {9, "LONG_TURBO"},
        {10, "LITE_FAST"}, {11, "LITE_SLOW"}, {12, "NARROW_FAST"},
        {13, "NARROW_SLOW"}, {14, "TINY_FAST"}, {15, "TINY_SLOW"},
        {16, "MEDIUM_TURBO"},
    };

    for(size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if(map[i].value == value) {
            return map[i].name;
        }
    }
    return nullptr;
}

static bool phoneapi_parse_lora_config_update(
    const std::vector<uint8_t> &payload, phoneapi_config_update_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    out->has_lora = true;
    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 2U && wire == 0U) {
            uint32_t value;
            const char *name;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            name = phoneapi_preset_name_from_enum(value);
            if(name) {
                out->preset = name;
                out->has_preset = true;
            }
        } else if(field == 7U && wire == 0U) {
            uint32_t value;
            const char *name;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            name = phoneapi_region_name_from_enum(value);
            if(name) {
                out->region = name;
                out->has_region = true;
            }
        } else if(field == 8U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->hop_limit)) {
                return false;
            }
            if(out->hop_limit > 7U) {
                out->hop_limit = 7U;
            }
            out->has_hop_limit = true;
        } else if(field == 10U && wire == 0U) {
            uint64_t value;
            if(!read_varint64(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->tx_power = (int32_t)value;
            out->has_tx_power = true;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_config_update(const std::vector<uint8_t> &payload,
                                         phoneapi_config_update_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    *out = phoneapi_config_update_t();
    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 6U && wire == 2U) {
            std::vector<uint8_t> lora;
            if(!phoneapi_read_length_delimited(payload, &pos, &lora) ||
               !phoneapi_parse_lora_config_update(lora, out)) {
                return false;
            }
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_queue_mesh_tx(const phoneapi_mesh_tx_t &tx)
{
    bool queued = false;

    pthread_mutex_lock(&phoneapi_tx_mutex);
    if(!phoneapi_pending_tx.active) {
        phoneapi_pending_tx = tx;
        phoneapi_pending_tx.active = true;
        queued = true;
    }
    pthread_mutex_unlock(&phoneapi_tx_mutex);
    return queued;
}

static bool phoneapi_take_mesh_tx(phoneapi_mesh_tx_t *tx)
{
    bool found = false;

    if(!tx) {
        return false;
    }
    pthread_mutex_lock(&phoneapi_tx_mutex);
    if(phoneapi_pending_tx.active) {
        *tx = phoneapi_pending_tx;
        phoneapi_pending_tx = phoneapi_mesh_tx_t();
        found = true;
    }
    pthread_mutex_unlock(&phoneapi_tx_mutex);
    return found;
}

static bool encode_phoneapi_mesh_packet_decoded(const mesh_header_t &header,
                                                const std::vector<uint8_t> &decoded,
                                                float rssi, float snr,
                                                std::vector<uint8_t> *out)
{
    uint32_t now = (uint32_t)time(nullptr);

    if(!out) {
        return false;
    }
    out->clear();
    append_varint(out, (1U << 3U) | 5U);
    append_fixed32(out, header.from);
    append_varint(out, (2U << 3U) | 5U);
    append_fixed32(out, header.to);
    append_uint32_field(out, 3U, 0U);
    append_bytes_field(out, 4U, decoded);
    append_varint(out, (6U << 3U) | 5U);
    append_fixed32(out, header.id);
    append_varint(out, (7U << 3U) | 5U);
    append_fixed32(out, now);
    append_float_field(out, 8U, snr);
    append_uint32_field(out, 9U, mesh_header_hop_limit(header));
    if(mesh_header_want_ack(header)) {
        append_bool_field(out, 10U, true);
    }
    if(rssi < 0.0f) {
        append_varint(out, (12U << 3U) | 0U);
        append_varint(out, (uint32_t)(int32_t)roundf(rssi));
    }
    append_uint32_field(out, 15U, mesh_header_hop_start(header));
    if(header.next_hop != 0U) {
        append_uint32_field(out, 18U, header.next_hop);
    }
    if(header.relay_node != 0U) {
        append_uint32_field(out, 19U, header.relay_node);
    }
    return true;
}

static bool phoneapi_send_from_payload_global(uint32_t field,
                                              const std::vector<uint8_t> &payload,
                                              const char *label)
{
    int fd = phoneapi_uart_fd;

    if(fd < 0 || !phoneapi_thread_running || !phoneapi_bridge_can_send()) {
        return false;
    }
    return phoneapi_send_from_payload(fd, field, payload, label);
}

static void phoneapi_notify_mesh_rx(const mesh_header_t &header,
                                    const std::vector<uint8_t> &decoded,
                                    float rssi, float snr)
{
    std::vector<uint8_t> packet;

    if(!encode_phoneapi_mesh_packet_decoded(header, decoded, rssi, snr,
                                            &packet)) {
        return;
    }
    (void)phoneapi_send_from_payload_global(2U, packet, "rx_packet");
}

static int phoneapi_open_uart(void)
{
    int fd = open(MESHTASTIC_PHONEAPI_UART_DEV,
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
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
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

static bool phoneapi_uart_send_line(int fd, const std::string &line)
{
    std::string wire = line + "\r\n";
    const char *ptr = wire.c_str();
    size_t left = wire.size();
    bool ok = true;

    pthread_mutex_lock(&phoneapi_uart_mutex);
    while(left > 0U) {
        ssize_t n = write(fd, ptr, left);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            ok = false;
            break;
        }
        ptr += n;
        left -= (size_t)n;
    }
    pthread_mutex_unlock(&phoneapi_uart_mutex);
    return ok;
}

static bool phoneapi_send_adv_start(int fd, const char *reason)
{
    std::string name = phoneapi_adv_name(phoneapi_opts);
    std::string command = "AT+MESHADV=" + name;
    bool ok = phoneapi_uart_send_line(fd, command);

    phoneapi_last_adv_us = monotonic_us();
    daemon_event("PhoneAPI bridge advertising name=%s reason=%s ok=%s",
                 name.c_str(), reason && reason[0] ? reason : "manual",
                 ok ? "yes" : "no");
    return ok;
}

static bool phoneapi_send_from_payload(int fd, uint32_t field,
                                       const std::vector<uint8_t> &payload,
                                       const char *label)
{
    std::vector<uint8_t> frame;
    std::string line;

    if(!encode_phoneapi_from_payload(field, payload, &frame)) {
        return false;
    }
    line = "AT+MESHFROM=" + phoneapi_hex_encode(frame);
    if(!phoneapi_uart_send_line(fd, line)) {
        daemon_event("PhoneAPI send %s failed", label ? label : "payload");
        return false;
    }
    daemon_event("PhoneAPI send %s bytes=%u", label ? label : "payload",
                 (unsigned)frame.size());
    usleep(25000);
    return true;
}

static bool encode_phoneapi_queue_status(uint32_t packet_id, uint32_t res,
                                         uint32_t free_slots,
                                         uint32_t max_slots,
                                         std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(out, 1U, res);
    append_uint32_field(out, 2U, free_slots);
    append_uint32_field(out, 3U, max_slots);
    append_uint32_field(out, 4U, packet_id);
    return true;
}

static bool phoneapi_send_queue_status(int fd, uint32_t packet_id,
                                       uint32_t res, uint32_t free_slots,
                                       const char *reason)
{
    std::vector<uint8_t> payload;
    const uint32_t max_slots = 1U;
    bool ok;

    if(!encode_phoneapi_queue_status(packet_id, res, free_slots, max_slots,
                                     &payload)) {
        return false;
    }
    ok = phoneapi_send_from_payload(fd, 11U, payload, "queue_status");
    daemon_event("PhoneAPI queue_status id=0x%08x res=%u free=%u reason=%s ok=%s",
                 packet_id, res, free_slots,
                 reason && reason[0] ? reason : "-",
                 ok ? "yes" : "no");
    return ok;
}

static void phoneapi_session_passkey(const probe_options_t &opts,
                                     uint8_t passkey[8])
{
    uint32_t name_hash = djb2_hash(opts.node_name.c_str());

    put_le32(passkey, opts.from_node);
    put_le32(passkey + 4, name_hash);
}

static bool encode_phoneapi_admin_response_bytes(const probe_options_t &opts,
                                                 uint32_t field,
                                                 const std::vector<uint8_t> &value,
                                                 std::vector<uint8_t> *out)
{
    uint8_t passkey[8];

    if(!out) {
        return false;
    }
    phoneapi_session_passkey(opts, passkey);
    out->clear();
    append_bytes_field(out, field, value);
    append_bytes_field(out, 101U, passkey, sizeof(passkey));
    return true;
}

static bool encode_phoneapi_admin_owner_response(const probe_options_t &opts,
                                                 std::vector<uint8_t> *out)
{
    std::vector<uint8_t> user;

    if(!out || !encode_phoneapi_user_proto(opts, &user)) {
        return false;
    }
    return encode_phoneapi_admin_response_bytes(opts, 4U, user, out);
}

static bool encode_phoneapi_admin_config_response(const probe_options_t &opts,
                                                  uint32_t config_type,
                                                  std::vector<uint8_t> *out)
{
    std::vector<uint8_t> config;
    bool ok = true;

    if(!out) {
        return false;
    }
    switch(config_type) {
    case 0U:
        ok = encode_phoneapi_config_device(&config);
        break;
    case 5U:
        ok = encode_phoneapi_config_lora(opts, &config);
        break;
    case 6U:
        ok = encode_phoneapi_config_bluetooth(&config);
        break;
    default:
        config.clear();
        daemon_event("PhoneAPI local admin config type %u returns empty",
                     config_type);
        break;
    }
    return ok && encode_phoneapi_admin_response_bytes(opts, 6U, config, out);
}

static bool encode_phoneapi_admin_module_config_response(
    const probe_options_t &opts, uint32_t module_config_type,
    std::vector<uint8_t> *out)
{
    std::vector<uint8_t> module_config;

    if(!out) {
        return false;
    }
    daemon_event("PhoneAPI local admin module config type %u returns empty",
                 module_config_type);
    return encode_phoneapi_admin_response_bytes(opts, 8U, module_config, out);
}

static bool encode_phoneapi_admin_channel_response(const probe_options_t &opts,
                                                   uint32_t channel_request,
                                                   std::vector<uint8_t> *out)
{
    std::vector<uint8_t> channel;

    if(!out || !encode_phoneapi_channel(opts, &channel)) {
        return false;
    }
    daemon_event("PhoneAPI local admin channel request=%u", channel_request);
    return encode_phoneapi_admin_response_bytes(opts, 2U, channel, out);
}

static bool encode_phoneapi_admin_metadata_response(const probe_options_t &opts,
                                                    std::vector<uint8_t> *out)
{
    std::vector<uint8_t> metadata;

    if(!out || !encode_phoneapi_metadata(&metadata)) {
        return false;
    }
    return encode_phoneapi_admin_response_bytes(opts, 13U, metadata, out);
}

#define K230_PHONE_UI_PREFS_DIR "/root/.config/k230_phone_ui"
#define K230_PHONE_UI_PREFS_FILE K230_PHONE_UI_PREFS_DIR "/settings.conf"
#define K230_PHONE_UI_PREFS_LOCK K230_PHONE_UI_PREFS_DIR "/settings.conf.lock"
#define K230_MESH_PREF_REGION "meshtastic.region"
#define K230_MESH_PREF_PRESET "meshtastic.preset"
#define K230_MESH_PREF_CHANNEL "meshtastic.channel"
#define K230_MESH_PREF_PSK "meshtastic.psk"
#define K230_MESH_PREF_POWER "meshtastic.power"
#define K230_MESH_PREF_NODE "meshtastic.node"
#define K230_MESH_PREF_FROM "meshtastic.from"
#define K230_MESH_PREF_TO "meshtastic.to"
#define K230_MESH_PREF_HOP "meshtastic.hop"
#define K230_MESH_PREF_ACK "meshtastic.ack"
#define K230_MESH_PREF_REBROADCAST "meshtastic.rebroadcast"
#define K230_PHONE_UI_PREF_VALUE_MAX 159U

typedef struct {
    std::string key;
    std::string value;
} phoneapi_pref_entry_t;

static std::string phoneapi_trim_copy(const char *text)
{
    const char *start = text ? text : "";
    const char *end;

    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while(end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return std::string(start, (size_t)(end - start));
}

static bool phoneapi_pref_key_valid(const std::string &key)
{
    if(key.empty() || key.size() >= 64U) {
        return false;
    }
    for(char c : key) {
        unsigned char ch = (unsigned char)c;

        if(isspace(ch) || ch == '=' || ch == '#') {
            return false;
        }
    }
    return true;
}

static std::string phoneapi_pref_value_clip(const std::string &value)
{
    if(value.size() <= K230_PHONE_UI_PREF_VALUE_MAX) {
        return value;
    }
    return value.substr(0, K230_PHONE_UI_PREF_VALUE_MAX);
}

static bool phoneapi_pref_load(std::vector<phoneapi_pref_entry_t> *entries)
{
    FILE *fp;
    char line[512];

    if(!entries) {
        return false;
    }
    entries->clear();
    fp = fopen(K230_PHONE_UI_PREFS_FILE, "r");
    if(!fp) {
        return errno == ENOENT;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *sep;
        std::string key;
        std::string value;

        line[strcspn(line, "\r\n")] = '\0';
        sep = strchr(line, '=');
        if(!sep) {
            continue;
        }
        *sep++ = '\0';
        key = phoneapi_trim_copy(line);
        value = phoneapi_trim_copy(sep);
        if(!phoneapi_pref_key_valid(key)) {
            continue;
        }
        entries->push_back({key, phoneapi_pref_value_clip(value)});
        if(entries->size() >= 96U) {
            break;
        }
    }
    fclose(fp);
    return true;
}

static void phoneapi_pref_set(std::vector<phoneapi_pref_entry_t> *entries,
                              const char *key, const std::string &value)
{
    std::string clipped = phoneapi_pref_value_clip(value);

    if(!entries || !key || !phoneapi_pref_key_valid(key)) {
        return;
    }
    for(size_t i = 0; i < entries->size(); i++) {
        if((*entries)[i].key == key) {
            (*entries)[i].value = clipped;
            return;
        }
    }
    entries->push_back({key, clipped});
}

static bool phoneapi_pref_write(const std::vector<phoneapi_pref_entry_t> &entries)
{
    char tmp_path[sizeof(K230_PHONE_UI_PREFS_FILE) + 8];
    FILE *fp;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
             K230_PHONE_UI_PREFS_FILE);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        return false;
    }
    fprintf(fp, "# k230_phone_ui persistent settings\n");
    for(size_t i = 0; i < entries.size(); i++) {
        if(phoneapi_pref_key_valid(entries[i].key)) {
            fprintf(fp, "%s=%s\n", entries[i].key.c_str(),
                    phoneapi_pref_value_clip(entries[i].value).c_str());
        }
    }
    if(fclose(fp) != 0) {
        unlink(tmp_path);
        return false;
    }
    if(rename(tmp_path, K230_PHONE_UI_PREFS_FILE) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

static bool phoneapi_persist_meshtastic_opts(const probe_options_t &opts)
{
    std::vector<phoneapi_pref_entry_t> entries;
    char value[64];
    int lock_fd;
    bool ok = false;
    std::string region = opts.region.empty() ? opts.resolved_region :
                         opts.region;
    std::string preset = opts.preset.empty() ? opts.resolved_preset :
                         opts.preset;

    if(region.empty()) {
        region = MESHTASTIC_DEFAULT_REGION;
    }
    if(preset.empty()) {
        preset = MESHTASTIC_DEFAULT_PRESET;
    }
    if(mkdir("/root/.config", 0755) != 0 && errno != EEXIST) {
        daemon_event("PhoneAPI prefs mkdir /root/.config failed: %s",
                     strerror(errno));
        return false;
    }
    if(mkdir(K230_PHONE_UI_PREFS_DIR, 0755) != 0 && errno != EEXIST) {
        daemon_event("PhoneAPI prefs mkdir failed: %s", strerror(errno));
        return false;
    }
    lock_fd = open(K230_PHONE_UI_PREFS_LOCK,
                   O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if(lock_fd < 0) {
        daemon_event("PhoneAPI prefs lock open failed: %s", strerror(errno));
        return false;
    }
    if(flock(lock_fd, LOCK_EX) != 0) {
        daemon_event("PhoneAPI prefs lock failed: %s", strerror(errno));
        close(lock_fd);
        return false;
    }
    if(phoneapi_pref_load(&entries)) {
        phoneapi_pref_set(&entries, K230_MESH_PREF_REGION, region);
        phoneapi_pref_set(&entries, K230_MESH_PREF_PRESET, preset);
        phoneapi_pref_set(&entries, K230_MESH_PREF_CHANNEL,
                          opts.channel_name);
        phoneapi_pref_set(&entries, K230_MESH_PREF_PSK, opts.psk);
        phoneapi_pref_set(&entries, K230_MESH_PREF_POWER,
                          opts.manual_power ?
                          std::to_string((int)opts.profile.power) : "auto");
        phoneapi_pref_set(&entries, K230_MESH_PREF_NODE, opts.node_name);
        snprintf(value, sizeof(value), "0x%08x", opts.from_node);
        phoneapi_pref_set(&entries, K230_MESH_PREF_FROM, value);
        snprintf(value, sizeof(value), "0x%08x", opts.to_node);
        phoneapi_pref_set(&entries, K230_MESH_PREF_TO, value);
        snprintf(value, sizeof(value), "%u", opts.hop_limit);
        phoneapi_pref_set(&entries, K230_MESH_PREF_HOP, value);
        phoneapi_pref_set(&entries, K230_MESH_PREF_ACK,
                          opts.want_ack ? "1" : "0");
        phoneapi_pref_set(&entries, K230_MESH_PREF_REBROADCAST,
                          opts.rebroadcast ? "1" : "0");
        ok = phoneapi_pref_write(entries);
    }
    if(!ok) {
        daemon_event("PhoneAPI prefs persist failed");
    }
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return ok;
}

static bool phoneapi_apply_admin_writes(const phoneapi_admin_request_t &admin,
                                        probe_options_t *opts,
                                        bool *request_reconfigure,
                                        bool *request_adv_refresh)
{
    bool ok_all = true;

    if(!opts || !request_reconfigure || !request_adv_refresh) {
        return false;
    }
    *request_reconfigure = false;
    *request_adv_refresh = false;

    if(admin.has_set_owner) {
        phoneapi_owner_update_t owner;
        bool ok = phoneapi_parse_user_update(admin.set_owner, &owner);

        if(ok && owner.has_name) {
            std::string clean = mesh_clean_text(owner.name);
            if(!clean.empty()) {
                opts->node_name = clean;
                *request_adv_refresh = true;
                daemon_event("PhoneAPI local admin set_owner name=%s",
                             opts->node_name.c_str());
            }
        }
        ok_all = ok_all && ok;
    }

    if(admin.has_set_channel) {
        phoneapi_channel_update_t channel;
        bool ok = phoneapi_parse_channel_update(admin.set_channel, &channel);

        if(ok) {
            if(channel.has_name) {
                opts->channel_name = mesh_clean_text(channel.name);
            }
            if(channel.has_psk) {
                opts->psk = channel.psk;
            }
            *request_reconfigure = true;
            daemon_event("PhoneAPI local admin set_channel name=%s psk=%s",
                         opts->channel_name.empty() ? "<preset>" :
                         opts->channel_name.c_str(),
                         opts->psk.c_str());
        }
        ok_all = ok_all && ok;
    }

    if(admin.has_set_config) {
        phoneapi_config_update_t config;
        bool ok = phoneapi_parse_config_update(admin.set_config, &config);

        if(ok && config.has_lora) {
            if(config.has_region) {
                opts->region = config.region;
            }
            if(config.has_preset) {
                opts->preset = config.preset;
            }
            if(config.has_hop_limit) {
                opts->hop_limit = config.hop_limit;
            }
            if(config.has_tx_power && config.tx_power >= -9 &&
               config.tx_power <= MESHTASTIC_MAX_K230_TX_POWER_DBM) {
                opts->profile.power = (int8_t)config.tx_power;
                opts->manual_power = config.tx_power != 0;
            }
            *request_reconfigure = true;
            daemon_event("PhoneAPI local admin set_config lora region=%s preset=%s hop=%u power=%d",
                         opts->region.c_str(), opts->preset.c_str(),
                         opts->hop_limit, opts->profile.power);
        }
        ok_all = ok_all && ok;
    }

    if(admin.has_set_module_config) {
        daemon_event("PhoneAPI local admin set_module_config ignored len=%u",
                     (unsigned)admin.set_module_config.size());
    }

    if(*request_reconfigure) {
        ok_all = apply_meshtastic_profile(opts) && ok_all;
    }
    return ok_all;
}

static bool phoneapi_send_local_admin_response(int fd,
                                               const phoneapi_mesh_tx_t &tx,
                                               const std::vector<uint8_t> &admin_payload,
                                               const char *label)
{
    std::vector<uint8_t> decoded;
    std::vector<uint8_t> packet;
    mesh_header_t header;

    if(!encode_data_proto(MESHTASTIC_ADMIN_APP, admin_payload,
                          tx.packet_id, 0U, &decoded)) {
        return false;
    }
    memset(&header, 0, sizeof(header));
    header.from = phoneapi_opts.from_node;
    header.to = tx.from_node != 0U ? tx.from_node : phoneapi_opts.from_node;
    header.id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
    if(header.id == 0U) {
        header.id = 1U;
    }
    if(!encode_phoneapi_mesh_packet_decoded(header, decoded, 0.0f, 0.0f,
                                            &packet)) {
        return false;
    }
    return phoneapi_send_from_payload(fd, 2U, packet,
                                      label ? label : "admin_response");
}

static bool phoneapi_handle_local_admin(int fd, const phoneapi_mesh_tx_t &tx,
                                        bool *accepted)
{
    phoneapi_admin_request_t admin;
    probe_options_t runtime_opts;
    bool handled = false;
    bool ok_all = true;
    bool request_reconfigure = false;
    bool request_adv_refresh = false;

    if(accepted) {
        *accepted = false;
    }

    if(tx.data.portnum != MESHTASTIC_ADMIN_APP) {
        return false;
    }
    if(tx.to_node != 0U && tx.to_node != phoneapi_opts.from_node) {
        return false;
    }
    if(!phoneapi_parse_admin_request(tx.data.payload, &admin)) {
        daemon_event("PhoneAPI local admin parse failed id=0x%08x payload=%u",
                     tx.packet_id, (unsigned)tx.data.payload.size());
        return true;
    }
    pthread_mutex_lock(&phoneapi_opts_mutex);
    runtime_opts = phoneapi_opts;
    pthread_mutex_unlock(&phoneapi_opts_mutex);

    if(admin.has_set_time_only) {
        daemon_event("PhoneAPI local admin set_time_only=%u id=0x%08x",
                     admin.set_time_only, tx.packet_id);
        handled = true;
    }
    if(admin.has_set_owner || admin.has_set_channel || admin.has_set_config ||
       admin.has_set_module_config) {
        bool persist_required = admin.has_set_owner || admin.has_set_channel ||
                                admin.has_set_config;
        bool persist_ok = true;
        bool ok = phoneapi_apply_admin_writes(admin, &runtime_opts,
                                              &request_reconfigure,
                                              &request_adv_refresh);
        if(ok) {
            phoneapi_store_runtime_opts(runtime_opts, request_reconfigure);
            if(persist_required) {
                persist_ok = phoneapi_persist_meshtastic_opts(runtime_opts);
            }
            if(request_adv_refresh) {
                (void)phoneapi_send_adv_start(fd, "admin-update");
            }
        }
        daemon_event("PhoneAPI local admin write id=0x%08x ok=%s reconfig=%s adv=%s persist=%s",
                     tx.packet_id, ok ? "yes" : "no",
                     request_reconfigure ? "yes" : "no",
                     request_adv_refresh ? "yes" : "no",
                     persist_ok ? "yes" : "no");
        ok_all = ok_all && ok && persist_ok;
        handled = true;
    }
    if(admin.get_owner_request) {
        std::vector<uint8_t> response;
        bool ok = encode_phoneapi_admin_owner_response(runtime_opts,
                                                       &response) &&
                  phoneapi_send_local_admin_response(fd, tx, response,
                                                     "admin_owner");
        daemon_event("PhoneAPI local admin owner_response id=0x%08x ok=%s",
                     tx.packet_id, ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_get_channel_request) {
        std::vector<uint8_t> response;
        bool ok = encode_phoneapi_admin_channel_response(
                      runtime_opts, admin.get_channel_request, &response) &&
                  phoneapi_send_local_admin_response(fd, tx, response,
                                                     "admin_channel");
        daemon_event("PhoneAPI local admin channel_response id=0x%08x req=%u ok=%s",
                     tx.packet_id, admin.get_channel_request,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_get_config_request) {
        std::vector<uint8_t> response;
        bool ok = encode_phoneapi_admin_config_response(
                      runtime_opts, admin.get_config_request, &response) &&
                  phoneapi_send_local_admin_response(fd, tx, response,
                                                     "admin_config");
        daemon_event("PhoneAPI local admin config_response id=0x%08x type=%u ok=%s",
                     tx.packet_id, admin.get_config_request,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_get_module_config_request) {
        std::vector<uint8_t> response;
        bool ok = encode_phoneapi_admin_module_config_response(
                      runtime_opts, admin.get_module_config_request,
                      &response) &&
                  phoneapi_send_local_admin_response(fd, tx, response,
                                                     "admin_module_config");
        daemon_event("PhoneAPI local admin module_config_response id=0x%08x type=%u ok=%s",
                     tx.packet_id, admin.get_module_config_request,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.get_device_metadata_request) {
        std::vector<uint8_t> response;
        bool ok = encode_phoneapi_admin_metadata_response(runtime_opts,
                                                          &response) &&
                  phoneapi_send_local_admin_response(fd, tx, response,
                                                     "admin_metadata");
        daemon_event("PhoneAPI local admin metadata_response id=0x%08x ok=%s",
                     tx.packet_id, ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(!handled) {
        daemon_event("PhoneAPI local admin unsupported id=0x%08x payload=%u",
                     tx.packet_id, (unsigned)tx.data.payload.size());
    }
    if(accepted) {
        *accepted = handled && ok_all;
    }
    return true;
}

static bool phoneapi_send_config_complete(int fd, uint32_t nonce)
{
    std::vector<uint8_t> frame;
    std::string line;

    if(!encode_phoneapi_config_complete(nonce, &frame)) {
        return false;
    }
    line = "AT+MESHFROM=" + phoneapi_hex_encode(frame);
    if(!phoneapi_uart_send_line(fd, line)) {
        daemon_event("PhoneAPI config_complete send failed nonce=%u", nonce);
        return false;
    }
    daemon_event("PhoneAPI config_complete nonce=%u", nonce);
    usleep(25000);
    return true;
}

static bool phoneapi_send_config_stage(int fd, const probe_options_t &opts,
                                       uint32_t nonce)
{
    std::vector<uint8_t> payload;
    bool ok = true;

    daemon_event("PhoneAPI config stage requested nonce=%u", nonce);
    ok = encode_phoneapi_my_node_info(opts, &payload) &&
         phoneapi_send_from_payload(fd, 3U, payload, "my_info") && ok;
    ok = encode_phoneapi_metadata(&payload) &&
         phoneapi_send_from_payload(fd, 13U, payload, "metadata") && ok;
    ok = encode_phoneapi_region_presets(&payload) &&
         phoneapi_send_from_payload(fd, 19U, payload, "region_presets") && ok;
    ok = encode_phoneapi_config_lora(opts, &payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_lora") && ok;
    ok = encode_phoneapi_config_device(&payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_device") && ok;
    ok = encode_phoneapi_config_bluetooth(&payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_bluetooth") && ok;
    ok = encode_phoneapi_channel(opts, &payload) &&
         phoneapi_send_from_payload(fd, 10U, payload, "channel") && ok;
    ok = phoneapi_send_config_complete(fd, nonce) && ok;
    return ok;
}

static bool phoneapi_send_nodeinfo_stage(int fd, const probe_options_t &opts,
                                         uint32_t nonce)
{
    std::vector<uint8_t> payload;
    bool ok = true;

    daemon_event("PhoneAPI nodeinfo stage requested nonce=%u", nonce);
    ok = encode_phoneapi_node_info(opts, &payload) &&
         phoneapi_send_from_payload(fd, 4U, payload, "node_info") && ok;
    ok = phoneapi_send_config_complete(fd, nonce) && ok;
    return ok;
}

static void phoneapi_process_toradio(int fd, const char *hex, size_t hex_len)
{
    std::vector<uint8_t> data;
    phoneapi_to_radio_t msg;

    if(!phoneapi_hex_decode(hex, hex_len, &data) ||
       !phoneapi_parse_to_radio(data.data(), data.size(), &msg)) {
        daemon_event("PhoneAPI ToRadio parse failed hex_len=%u",
                     (unsigned)hex_len);
        return;
    }
    if(msg.has_want_config) {
        if(msg.want_config_id == MESHTASTIC_PHONEAPI_CONFIG_NONCE) {
            (void)phoneapi_send_config_stage(fd, phoneapi_opts,
                                             msg.want_config_id);
        } else if(msg.want_config_id == MESHTASTIC_PHONEAPI_NODEINFO_NONCE) {
            (void)phoneapi_send_nodeinfo_stage(fd, phoneapi_opts,
                                               msg.want_config_id);
        } else if(msg.want_config_id == 0U) {
            daemon_event("PhoneAPI ignoring empty want_config_id=0");
        } else {
            daemon_event("PhoneAPI ignoring unknown want_config_id=%u",
                         msg.want_config_id);
        }
    }
    if(msg.packet_len > 0U) {
        phoneapi_mesh_tx_t tx;
        if(phoneapi_parse_mesh_packet(msg.packet, &tx)) {
            bool local_admin_ok = false;
            if(phoneapi_handle_local_admin(fd, tx, &local_admin_ok)) {
                daemon_event("PhoneAPI ToRadio local admin handled len=%u id=0x%08x ok=%s",
                             (unsigned)msg.packet_len, tx.packet_id,
                             local_admin_ok ? "yes" : "no");
                (void)phoneapi_send_queue_status(
                    fd, tx.packet_id, local_admin_ok ? 0U : 1U,
                    local_admin_ok ? 1U : 0U,
                    local_admin_ok ? "local-admin" : "local-admin-failed");
            } else if(phoneapi_queue_mesh_tx(tx)) {
                daemon_event("PhoneAPI ToRadio packet queued len=%u port=%u to=0x%08x ack=%s",
                             (unsigned)msg.packet_len, tx.data.portnum,
                             tx.to_node, tx.want_ack ? "on" : "off");
                (void)phoneapi_send_queue_status(fd, tx.packet_id, 0U, 1U,
                                                  "queued");
            } else {
                daemon_event("PhoneAPI ToRadio packet dropped queue busy len=%u",
                             (unsigned)msg.packet_len);
                (void)phoneapi_send_queue_status(fd, tx.packet_id, 1U, 0U,
                                                  "busy");
            }
        } else {
            daemon_event("PhoneAPI ToRadio packet unsupported len=%u",
                         (unsigned)msg.packet_len);
            (void)phoneapi_send_queue_status(fd, 0U, 1U, 0U,
                                              "unsupported");
        }
    }
    if(msg.heartbeat) {
        daemon_event("PhoneAPI heartbeat");
        (void)phoneapi_send_queue_status(fd, 0U, 0U, 1U, "heartbeat");
    }
    if(msg.disconnect) {
        daemon_event("PhoneAPI disconnect");
    }
}

static void phoneapi_process_uart_line(int fd, const std::string &raw_line)
{
    std::string line = raw_line;

    while(!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                            isspace((unsigned char)line.back()))) {
        line.pop_back();
    }
    if(line.empty() || line == "OK") {
        return;
    }
    if(phoneapi_bridge_status_line(line)) {
        bool connected = phoneapi_bridge_status_connected(line);
        bool adv_known = false;
        bool advertising = phoneapi_bridge_status_advertising(line,
                                                              &adv_known);
        phoneapi_bridge_state_t state = connected ?
            PHONEAPI_BRIDGE_CONNECTED : PHONEAPI_BRIDGE_READY;
        const char *detail = connected ? "connected" :
                             (adv_known && !advertising ? "idle" :
                              "advertising");

        phoneapi_bridge_set_state(state, detail);
        if(!phoneapi_init_sent) {
            phoneapi_init_sent = true;
            phoneapi_last_adv_us = 0;
            daemon_event("PhoneAPI bridge supported: %s", line.c_str());
            (void)phoneapi_uart_send_line(fd, "AT+MESHCLR");
            usleep(20000);
            (void)phoneapi_send_adv_start(fd, "init");
        } else {
            daemon_event("PhoneAPI UART %s", line.c_str());
            if(!connected && adv_known && !advertising) {
                (void)phoneapi_send_adv_start(fd, "status-idle");
            }
        }
        return;
    }
    if((line.rfind("ERR", 0) == 0 || line.rfind("+ERR", 0) == 0) &&
       !phoneapi_init_sent) {
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_UNSUPPORTED,
                                  "firmware-mismatch");
        daemon_event("PhoneAPI bridge disabled: unsupported nRF52840 firmware line=%s",
                     line.c_str());
        phoneapi_thread_running = false;
        return;
    }
    if(line.rfind("+MESH:TORADIO,", 0) == 0) {
        const char *start = line.c_str() + strlen("+MESH:TORADIO,");
        char *endptr = nullptr;
        unsigned long declared_len = strtoul(start, &endptr, 10);
        const char *hex = endptr && *endptr == ',' ? endptr + 1 : nullptr;
        size_t hex_len = hex ? strlen(hex) : 0U;

        if(!hex || declared_len * 2UL != hex_len) {
            daemon_event("PhoneAPI ToRadio length mismatch declared=%lu hex=%u",
                         declared_len, (unsigned)hex_len);
            return;
        }
        phoneapi_process_toradio(fd, hex, hex_len);
        return;
    }
    if(line.rfind("+MESH:", 0) == 0 || line.rfind("+ERR", 0) == 0 ||
       line.rfind("ERR", 0) == 0) {
        daemon_event("PhoneAPI UART %s", line.c_str());
    }
}

static void *phoneapi_thread_main(void *arg)
{
    (void)arg;
    int fd = phoneapi_open_uart();
    char line[2304];
    size_t line_len = 0;
    uint64_t last_probe_us = 0;
    uint64_t last_status_us = 0;
    unsigned int probe_attempts = 0;

    if(fd < 0) {
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_OFFLINE, "open-failed");
        daemon_event("PhoneAPI bridge disabled: open %s failed: %s",
                     MESHTASTIC_PHONEAPI_UART_DEV, strerror(errno));
        phoneapi_thread_running = false;
        return nullptr;
    }
    phoneapi_init_sent = false;
    pthread_mutex_lock(&phoneapi_uart_mutex);
    phoneapi_uart_fd = fd;
    pthread_mutex_unlock(&phoneapi_uart_mutex);
    phoneapi_bridge_set_state(PHONEAPI_BRIDGE_PROBING, "uart-open");
    daemon_event("PhoneAPI bridge probing uart=%s", MESHTASTIC_PHONEAPI_UART_DEV);
    (void)phoneapi_uart_send_line(fd, "AT+MESHSTATUS?");
    last_probe_us = monotonic_us();
    probe_attempts = 1;

    while(phoneapi_thread_running && running) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, 250);
        if(ret < 0) {
            if(errno == EINTR) {
                continue;
            }
            daemon_event("PhoneAPI poll failed: %s", strerror(errno));
            break;
        }
        if(ret == 0 || !(pfd.revents & POLLIN)) {
            uint64_t now = monotonic_us();
            if(!phoneapi_init_sent) {
                if(now - last_probe_us > 1000000ULL) {
                    if(probe_attempts >= 3U) {
                        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_UNSUPPORTED,
                                                  "status-timeout");
                        daemon_event("PhoneAPI bridge disabled: AT+MESHSTATUS? timeout");
                        phoneapi_thread_running = false;
                        break;
                    }
                    (void)phoneapi_uart_send_line(fd, "AT+MESHSTATUS?");
                    last_probe_us = now;
                    probe_attempts++;
                }
            } else {
                phoneapi_bridge_state_t bridge_state;

                if(now - last_status_us > 3000000ULL) {
                    (void)phoneapi_uart_send_line(fd, "AT+MESHSTATUS?");
                    last_status_us = now;
                }
                bridge_state = phoneapi_bridge_get_state(nullptr, 0);
                if(bridge_state != PHONEAPI_BRIDGE_CONNECTED &&
                   now - phoneapi_last_adv_us >
                   MESHTASTIC_PHONEAPI_ADV_REFRESH_US) {
                    (void)phoneapi_send_adv_start(fd, "refresh");
                }
            }
            continue;
        }
        for(;;) {
            char c;
            ssize_t n = read(fd, &c, 1);
            if(n < 0) {
                if(errno == EINTR) {
                    continue;
                }
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                phoneapi_bridge_set_state(PHONEAPI_BRIDGE_ERROR, "read-failed");
                daemon_event("PhoneAPI read failed: %s", strerror(errno));
                phoneapi_thread_running = false;
                break;
            }
            if(n == 0) {
                break;
            }
            if(c == '\n') {
                line[line_len] = 0;
                phoneapi_process_uart_line(fd, line);
                line_len = 0;
            } else if(c != '\r') {
                if(line_len + 1U < sizeof(line)) {
                    line[line_len++] = c;
                } else {
                    line_len = 0;
                    daemon_event("PhoneAPI UART line overflow");
                }
            }
        }
    }
    if(phoneapi_init_sent) {
        (void)phoneapi_uart_send_line(fd, "AT+MESHADV=OFF");
    }
    pthread_mutex_lock(&phoneapi_uart_mutex);
    phoneapi_uart_fd = -1;
    pthread_mutex_unlock(&phoneapi_uart_mutex);
    close(fd);
    if(phoneapi_bridge_get_state(nullptr, 0) != PHONEAPI_BRIDGE_UNSUPPORTED &&
       phoneapi_bridge_get_state(nullptr, 0) != PHONEAPI_BRIDGE_ERROR) {
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_OFFLINE, "stopped");
    }
    daemon_event("PhoneAPI bridge stopped");
    return nullptr;
}

static void phoneapi_start(const probe_options_t &opts)
{
    if(phoneapi_thread_started) {
        return;
    }
    phoneapi_store_runtime_opts(opts, false);
    phoneapi_bridge_set_state(PHONEAPI_BRIDGE_PROBING, "starting");
    phoneapi_thread_running = true;
    if(pthread_create(&phoneapi_thread, nullptr, phoneapi_thread_main,
                      nullptr) != 0) {
        phoneapi_thread_running = false;
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_ERROR, "thread-create");
        daemon_event("PhoneAPI bridge pthread_create failed: %s",
                     strerror(errno));
        return;
    }
    phoneapi_thread_started = true;
}

static void phoneapi_stop(void)
{
    if(!phoneapi_thread_started) {
        return;
    }
    phoneapi_thread_running = false;
    pthread_join(phoneapi_thread, nullptr);
    phoneapi_thread_started = false;
}

static bool encode_text_data_proto(const std::string &message,
                                   std::vector<uint8_t> *out)
{
    size_t payload_len = message.size();
    std::vector<uint8_t> payload;

    if(!out) {
        return false;
    }
    if(payload_len > MESHTASTIC_DATA_PAYLOAD_LEN) {
        payload_len = MESHTASTIC_DATA_PAYLOAD_LEN;
    }
    payload.assign(message.begin(), message.begin() + payload_len);
    return encode_data_proto(MESHTASTIC_TEXT_MESSAGE_APP, payload, 0, 0, out);
}

static bool decode_data_proto(const uint8_t *data, size_t len,
                              mesh_data_proto_t *decoded)
{
    size_t pos = 0;
    uint32_t found_port = 0;
    std::vector<uint8_t> found_payload;
    uint32_t request_id = 0;
    uint32_t reply_id = 0;
    bool want_response = false;

    while(pos < len) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(data, len, &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 1U && wire == 0U) {
            if(!read_varint(data, len, &pos, &found_port)) {
                return false;
            }
        } else if(field == 2U && wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            found_payload.assign(data + pos, data + pos + l);
            pos += l;
        } else if(field == 3U && wire == 0U) {
            uint32_t value;
            if(!read_varint(data, len, &pos, &value)) {
                return false;
            }
            want_response = value != 0U;
        } else if(field == 6U && wire == 5U && pos + 4U <= len) {
            request_id = get_le32(data + pos);
            pos += 4U;
        } else if(field == 7U && wire == 5U && pos + 4U <= len) {
            reply_id = get_le32(data + pos);
            pos += 4U;
        } else if(wire == 0U) {
            uint32_t ignored;
            if(!read_varint(data, len, &pos, &ignored)) {
                return false;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= len) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= len) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(decoded) {
        decoded->portnum = found_port;
        decoded->payload = found_payload;
        decoded->request_id = request_id;
        decoded->reply_id = reply_id;
        decoded->want_response = want_response;
    }
    return found_port != 0U;
}

static bool decode_user_proto(const std::vector<uint8_t> &payload,
                              mesh_user_info_t *user)
{
    size_t pos = 0;
    mesh_user_info_t found;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if((field == 2U || field == 3U) && wire == 2U) {
            uint32_t l;
            std::string text;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            text.assign((const char *)payload.data() + pos, l);
            pos += l;
            if(field == 2U) {
                found.long_name = mesh_clean_text(text);
            } else {
                found.short_name = mesh_clean_text(text);
            }
            found.has_name = true;
        } else if(field == 5U && wire == 0U) {
            uint32_t hw = 0;
            if(!read_varint(payload.data(), payload.size(), &pos, &hw)) {
                return false;
            }
            found.hw_model = (int)hw;
        } else if(wire == 0U) {
            uint32_t ignored;
            if(!read_varint(payload.data(), payload.size(), &pos, &ignored)) {
                return false;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= payload.size()) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= payload.size()) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(user) {
        *user = found;
    }
    return found.has_name || found.hw_model >= 0;
}

static bool decode_position_proto(const std::vector<uint8_t> &payload,
                                  mesh_position_info_t *position)
{
    size_t pos = 0;
    mesh_position_info_t found;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;

        if((field == 1U || field == 2U || field == 4U || field == 7U) &&
           wire == 5U && pos + 4U <= payload.size()) {
            uint32_t value = get_le32(payload.data() + pos);
            pos += 4U;
            if(field == 1U) {
                found.latitude_i = (int32_t)value;
                found.has_latitude = true;
            } else if(field == 2U) {
                found.longitude_i = (int32_t)value;
                found.has_longitude = true;
            } else if(field == 7U) {
                found.timestamp = value;
            }
        } else if(wire == 0U) {
            uint64_t value64 = 0;
            if(!read_varint64(payload.data(), payload.size(), &pos,
                              &value64)) {
                return false;
            }
            if(field == 3U) {
                found.altitude_m = (int32_t)(uint32_t)value64;
                found.has_altitude = true;
            } else if(field == 15U) {
                found.ground_speed_cms = (uint32_t)value64;
                found.has_ground_speed = true;
            } else if(field == 16U) {
                found.ground_track_1e5 = (uint32_t)value64;
                found.has_ground_track = true;
            } else if(field == 19U) {
                found.sats_in_view = (uint32_t)value64;
            } else if(field == 23U) {
                found.precision_bits = (uint32_t)value64;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= payload.size()) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= payload.size()) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(position) {
        *position = found;
    }
    return found.has_latitude && found.has_longitude;
}

static bool decode_device_metrics_proto(const uint8_t *data, size_t len,
                                        mesh_telemetry_info_t *telemetry)
{
    size_t pos = 0;
    bool found = false;

    if(!data || !telemetry) {
        return false;
    }
    while(pos < len) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(data, len, &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(wire == 0U) {
            uint64_t value64 = 0;
            if(!read_varint64(data, len, &pos, &value64)) {
                return false;
            }
            if(field == 1U) {
                telemetry->has_battery_level = true;
                telemetry->battery_level = (uint32_t)value64;
                found = true;
            } else if(field == 5U) {
                telemetry->uptime_seconds = (uint32_t)value64;
                found = true;
            }
        } else if(wire == 5U && pos + 4U <= len) {
            float value = fixed32_to_float(get_le32(data + pos));
            pos += 4U;
            if(field == 2U) {
                telemetry->has_device_voltage = true;
                telemetry->device_voltage = value;
                found = true;
            } else if(field == 3U) {
                telemetry->has_channel_utilization = true;
                telemetry->channel_utilization = value;
                found = true;
            } else if(field == 4U) {
                telemetry->has_air_util_tx = true;
                telemetry->air_util_tx = value;
                found = true;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= len) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= len) {
            pos += 8U;
        } else {
            return false;
        }
    }
    telemetry->has_device_metrics = telemetry->has_device_metrics || found;
    return found;
}

static bool decode_environment_metrics_proto(const uint8_t *data, size_t len,
                                             mesh_telemetry_info_t *telemetry)
{
    size_t pos = 0;
    bool found = false;

    if(!data || !telemetry) {
        return false;
    }
    while(pos < len) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(data, len, &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(wire == 0U) {
            uint64_t value64 = 0;
            if(!read_varint64(data, len, &pos, &value64)) {
                return false;
            }
            if(field == 7U) {
                telemetry->has_iaq = true;
                telemetry->iaq = (uint32_t)value64;
                found = true;
            }
        } else if(wire == 5U && pos + 4U <= len) {
            float value = fixed32_to_float(get_le32(data + pos));
            pos += 4U;
            if(field == 1U) {
                telemetry->has_temperature = true;
                telemetry->temperature_c = value;
                found = true;
            } else if(field == 2U) {
                telemetry->has_humidity = true;
                telemetry->humidity_percent = value;
                found = true;
            } else if(field == 3U) {
                telemetry->has_pressure = true;
                telemetry->pressure_hpa = value;
                found = true;
            } else if(field == 5U) {
                telemetry->has_environment_voltage = true;
                telemetry->environment_voltage = value;
                found = true;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= len) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= len) {
            pos += 8U;
        } else {
            return false;
        }
    }
    telemetry->has_environment_metrics =
        telemetry->has_environment_metrics || found;
    return found;
}

static bool decode_telemetry_proto(const std::vector<uint8_t> &payload,
                                   mesh_telemetry_info_t *telemetry)
{
    size_t pos = 0;
    mesh_telemetry_info_t found;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 1U && wire == 5U && pos + 4U <= payload.size()) {
            found.timestamp = get_le32(payload.data() + pos);
            pos += 4U;
        } else if((field == 2U || field == 3U) && wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            if(field == 2U) {
                (void)decode_device_metrics_proto(payload.data() + pos, l,
                                                  &found);
            } else {
                (void)decode_environment_metrics_proto(payload.data() + pos,
                                                       l, &found);
            }
            pos += l;
        } else if(wire == 0U) {
            uint64_t ignored = 0;
            if(!read_varint64(payload.data(), payload.size(), &pos,
                              &ignored)) {
                return false;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= payload.size()) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= payload.size()) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(telemetry) {
        *telemetry = found;
    }
    return found.has_device_metrics || found.has_environment_metrics;
}

static void summary_addf(std::string *out, const char *fmt, ...)
{
    char part[64];
    va_list ap;

    if(!out || !fmt) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(part, sizeof(part), fmt, ap);
    va_end(ap);
    if(part[0] == '\0') {
        return;
    }
    if(!out->empty()) {
        *out += " ";
    }
    *out += part;
}

static std::string telemetry_summary(const mesh_telemetry_info_t &telemetry)
{
    std::string out;

    if(telemetry.has_battery_level) {
        summary_addf(&out, "bat=%u%%", telemetry.battery_level);
    }
    if(telemetry.has_device_voltage) {
        summary_addf(&out, "v=%.2fV", telemetry.device_voltage);
    }
    if(telemetry.has_channel_utilization) {
        summary_addf(&out, "ch=%.1f%%", telemetry.channel_utilization);
    }
    if(telemetry.has_air_util_tx) {
        summary_addf(&out, "air=%.1f%%", telemetry.air_util_tx);
    }
    if(telemetry.has_temperature) {
        summary_addf(&out, "temp=%.1fC", telemetry.temperature_c);
    }
    if(telemetry.has_humidity) {
        summary_addf(&out, "hum=%.1f%%", telemetry.humidity_percent);
    }
    if(telemetry.has_pressure) {
        summary_addf(&out, "press=%.1fhPa", telemetry.pressure_hpa);
    }
    if(telemetry.has_environment_voltage) {
        summary_addf(&out, "env_v=%.2fV", telemetry.environment_voltage);
    }
    if(telemetry.has_iaq) {
        summary_addf(&out, "iaq=%u", telemetry.iaq);
    }
    if(telemetry.uptime_seconds != 0U) {
        summary_addf(&out, "up=%us", telemetry.uptime_seconds);
    }
    if(telemetry.timestamp != 0U) {
        summary_addf(&out, "time=%u", telemetry.timestamp);
    }
    if(out.empty()) {
        if(telemetry.has_device_metrics && telemetry.has_environment_metrics) {
            out = "device env";
        } else if(telemetry.has_device_metrics) {
            out = "device";
        } else if(telemetry.has_environment_metrics) {
            out = "env";
        } else {
            out = "-";
        }
    }
    return out;
}

static std::string telemetry_summary(const mesh_node_entry_t &node)
{
    mesh_telemetry_info_t telemetry;

    telemetry.has_device_metrics = node.has_device_metrics;
    telemetry.has_battery_level = node.has_battery_level;
    telemetry.has_device_voltage = node.has_device_voltage;
    telemetry.has_channel_utilization = node.has_channel_utilization;
    telemetry.has_air_util_tx = node.has_air_util_tx;
    telemetry.battery_level = node.battery_level;
    telemetry.uptime_seconds = node.uptime_seconds;
    telemetry.device_voltage = node.device_voltage;
    telemetry.channel_utilization = node.channel_utilization;
    telemetry.air_util_tx = node.air_util_tx;
    telemetry.has_environment_metrics = node.has_environment_metrics;
    telemetry.has_temperature = node.has_temperature;
    telemetry.has_humidity = node.has_humidity;
    telemetry.has_pressure = node.has_pressure;
    telemetry.has_environment_voltage = node.has_environment_voltage;
    telemetry.has_iaq = node.has_iaq;
    telemetry.temperature_c = node.temperature_c;
    telemetry.humidity_percent = node.humidity_percent;
    telemetry.pressure_hpa = node.pressure_hpa;
    telemetry.environment_voltage = node.environment_voltage;
    telemetry.iaq = node.iaq;
    telemetry.timestamp = node.telemetry_timestamp;
    return telemetry_summary(telemetry);
}

static bool decode_neighbor_entry_proto(const uint8_t *data, size_t len,
                                        mesh_neighbor_entry_info_t *neighbor)
{
    size_t pos = 0;
    mesh_neighbor_entry_info_t found;

    if(!data) {
        return false;
    }
    while(pos < len) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(data, len, &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(wire == 0U) {
            uint64_t value64 = 0;
            if(!read_varint64(data, len, &pos, &value64)) {
                return false;
            }
            if(field == 1U) {
                found.node_id = (uint32_t)value64;
                found.has_node_id = true;
            } else if(field == 4U) {
                found.broadcast_interval_secs = (uint32_t)value64;
            }
        } else if(wire == 5U && pos + 4U <= len) {
            uint32_t value = get_le32(data + pos);
            pos += 4U;
            if(field == 2U) {
                found.snr = fixed32_to_float(value);
                found.has_snr = true;
            } else if(field == 3U) {
                found.last_rx_time = value;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(data, len, &pos, &l) || pos + l > len) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= len) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= len) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(neighbor) {
        *neighbor = found;
    }
    return found.has_node_id;
}

static bool decode_neighbor_info_proto(const std::vector<uint8_t> &payload,
                                       mesh_neighbor_info_t *info)
{
    size_t pos = 0;
    mesh_neighbor_info_t found;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(wire == 0U) {
            uint64_t value64 = 0;
            if(!read_varint64(payload.data(), payload.size(), &pos,
                              &value64)) {
                return false;
            }
            if(field == 1U) {
                found.node_id = (uint32_t)value64;
                found.has_neighbor_info = true;
            } else if(field == 2U) {
                found.last_sent_by_id = (uint32_t)value64;
                found.has_neighbor_info = true;
            } else if(field == 3U) {
                found.broadcast_interval_secs = (uint32_t)value64;
                found.has_neighbor_info = true;
            }
        } else if(field == 4U && wire == 2U) {
            uint32_t l;
            mesh_neighbor_entry_info_t neighbor;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            if(decode_neighbor_entry_proto(payload.data() + pos, l,
                                           &neighbor)) {
                if(neighbor.has_snr) {
                    summary_addf(&found.summary, "0x%08x:%.1f",
                                 neighbor.node_id, neighbor.snr);
                } else {
                    summary_addf(&found.summary, "0x%08x",
                                 neighbor.node_id);
                }
                found.neighbor_count++;
                found.has_neighbor_info = true;
            }
            pos += l;
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= payload.size()) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= payload.size()) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(found.summary.empty() && found.has_neighbor_info) {
        found.summary = "-";
    }
    if(info) {
        *info = found;
    }
    return found.has_neighbor_info;
}

static bool decode_routing_error_proto(const std::vector<uint8_t> &payload,
                                       uint32_t *error_reason)
{
    size_t pos = 0;
    uint32_t found_error = MESHTASTIC_ROUTING_ERROR_NONE;
    bool found = false;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 3U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &found_error)) {
                return false;
            }
            found = true;
        } else if(wire == 0U) {
            uint32_t ignored;
            if(!read_varint(payload.data(), payload.size(), &pos, &ignored)) {
                return false;
            }
        } else if(wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            pos += l;
        } else if(wire == 5U && pos + 4U <= payload.size()) {
            pos += 4U;
        } else if(wire == 1U && pos + 8U <= payload.size()) {
            pos += 8U;
        } else {
            return false;
        }
    }

    if(error_reason) {
        *error_reason = found_error;
    }
    return found;
}

static bool aes_ctr_crypt(const std::vector<uint8_t> &key, uint32_t from_node,
                          uint32_t packet_id, std::vector<uint8_t> *bytes)
{
    EVP_CIPHER_CTX *ctx;
    const EVP_CIPHER *cipher;
    uint8_t nonce[16] = {0};
    int out_len = 0;
    int final_len = 0;
    std::vector<uint8_t> out;

    if(!bytes || key.empty()) {
        return true;
    }
    if(key.size() == 16U) {
        cipher = EVP_aes_128_ctr();
    } else if(key.size() == 32U) {
        cipher = EVP_aes_256_ctr();
    } else {
        return false;
    }

    put_le32(nonce, packet_id);
    put_le32(nonce + 4, 0);
    put_le32(nonce + 8, from_node);
    put_le32(nonce + 12, 0);

    ctx = EVP_CIPHER_CTX_new();
    if(!ctx) {
        return false;
    }
    out.resize(bytes->size() + 16U);
    if(EVP_EncryptInit_ex(ctx, cipher, nullptr, key.data(), nonce) != 1 ||
       EVP_EncryptUpdate(ctx, out.data(), &out_len, bytes->data(),
                         (int)bytes->size()) != 1 ||
       EVP_EncryptFinal_ex(ctx, out.data() + out_len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize((size_t)(out_len + final_len));
    *bytes = out;
    return true;
}

static bool parse_mesh_header(const uint8_t *data, size_t len,
                              mesh_header_t *header)
{
    if(!data || len < MESHTASTIC_HEADER_LENGTH || !header) {
        return false;
    }
    header->to = get_le32(data);
    header->from = get_le32(data + 4);
    header->id = get_le32(data + 8);
    header->flags = data[12];
    header->channel = data[13];
    header->next_hop = data[14];
    header->relay_node = data[15];
    return true;
}

static void append_mesh_header(std::vector<uint8_t> *out,
                               const mesh_header_t &header)
{
    size_t start = out->size();

    out->resize(start + MESHTASTIC_HEADER_LENGTH);
    put_le32(out->data() + start, header.to);
    put_le32(out->data() + start + 4U, header.from);
    put_le32(out->data() + start + 8U, header.id);
    (*out)[start + 12U] = header.flags;
    (*out)[start + 13U] = header.channel;
    (*out)[start + 14U] = header.next_hop;
    (*out)[start + 15U] = header.relay_node;
}

static uint8_t mesh_header_hop_limit(const mesh_header_t &header)
{
    return header.flags & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK;
}

static uint8_t mesh_header_hop_start(const mesh_header_t &header)
{
    return (header.flags & MESHTASTIC_PACKET_FLAGS_HOP_START_MASK) >>
           MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT;
}

static bool mesh_header_want_ack(const mesh_header_t &header)
{
    return (header.flags & MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK) != 0U;
}

static void mesh_header_set_hop_limit(mesh_header_t *header, uint8_t hop_limit)
{
    if(!header) {
        return;
    }
    header->flags =
        (uint8_t)((header->flags & ~MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                  (hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK));
}

static void mesh_history_expire(uint64_t now_us)
{
    size_t out = 0;

    for(size_t i = 0; i < mesh_history_count; i++) {
        uint64_t seen = mesh_history[i].seen_us;
        if(seen > now_us ||
           now_us - seen <= MESHTASTIC_PACKET_HISTORY_TTL_US) {
            if(out != i) {
                mesh_history[out] = mesh_history[i];
            }
            out++;
        }
    }
    if(out != mesh_history_count) {
        mesh_history_count = out;
        mesh_history_next = out % MESHTASTIC_PACKET_HISTORY_SIZE;
    }
}

static bool mesh_history_contains(const mesh_header_t &header)
{
    mesh_history_expire(monotonic_us());
    for(size_t i = 0; i < mesh_history_count; i++) {
        if(mesh_history[i].from == header.from &&
           mesh_history[i].id == header.id &&
           mesh_history[i].channel == header.channel) {
            return true;
        }
    }
    return false;
}

static void mesh_history_remember(const mesh_header_t &header)
{
    size_t index;

    if(mesh_history_contains(header)) {
        return;
    }

    if(mesh_history_count < MESHTASTIC_PACKET_HISTORY_SIZE) {
        index = mesh_history_count++;
    } else {
        index = mesh_history_next;
        mesh_history_next = (mesh_history_next + 1U) %
                            MESHTASTIC_PACKET_HISTORY_SIZE;
    }

    mesh_history[index].from = header.from;
    mesh_history[index].id = header.id;
    mesh_history[index].channel = header.channel;
    mesh_history[index].seen_us = monotonic_us();
}

static void mesh_history_remember_tx(const tx_frame_t &frame)
{
    mesh_header_t header;

    if(frame.packet_id == 0U || frame.bytes.size() < MESHTASTIC_HEADER_LENGTH) {
        return;
    }
    if(parse_mesh_header(frame.bytes.data(), frame.bytes.size(), &header)) {
        mesh_history_remember(header);
    }
}

static bool mesh_chat_text_seen_recently(uint32_t from, uint32_t to,
                                         const std::string &text)
{
    uint64_t now = monotonic_us();

    if(text.empty()) {
        return false;
    }
    for(size_t i = 0; i < MESHTASTIC_CHAT_DEDUP_SIZE; i++) {
        if(mesh_chat_dedup[i].seen_us == 0ULL ||
           now < mesh_chat_dedup[i].seen_us ||
           now - mesh_chat_dedup[i].seen_us > MESHTASTIC_CHAT_DEDUP_TTL_US) {
            continue;
        }
        if(mesh_chat_dedup[i].from == from && mesh_chat_dedup[i].to == to &&
           strncmp(mesh_chat_dedup[i].text, text.c_str(),
                   sizeof(mesh_chat_dedup[i].text)) == 0) {
            mesh_chat_dedup[i].seen_us = now;
            return true;
        }
    }

    mesh_chat_dedup[mesh_chat_dedup_next].from = from;
    mesh_chat_dedup[mesh_chat_dedup_next].to = to;
    mesh_chat_dedup[mesh_chat_dedup_next].seen_us = now;
    snprintf(mesh_chat_dedup[mesh_chat_dedup_next].text,
             sizeof(mesh_chat_dedup[mesh_chat_dedup_next].text), "%s",
             text.c_str());
    mesh_chat_dedup_next =
        (mesh_chat_dedup_next + 1U) % MESHTASTIC_CHAT_DEDUP_SIZE;
    return false;
}

static uint32_t mesh_prng_u32(uint32_t salt)
{
    static uint32_t state;

    if(state == 0U) {
        state = (uint32_t)(monotonic_us() & 0xffffffffU) ^
                ((uint32_t)getpid() << 16) ^ 0x4d455348U;
    }
    state ^= salt + 0x9e3779b9U + (state << 6U) + (state >> 2U);
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

static uint64_t mesh_rebroadcast_delay_us(const tx_frame_t &frame)
{
    uint32_t salt = frame.packet_id ^ frame.rebroadcast_from ^
                    ((uint32_t)frame.channel << 24U);
    uint64_t jitter = mesh_prng_u32(salt) %
                      (MESHTASTIC_REBROADCAST_JITTER_US + 1ULL);

    return MESHTASTIC_REBROADCAST_MIN_DELAY_US + jitter;
}

static unsigned int mesh_delayed_tx_count(void)
{
    unsigned int count = 0;

    for(size_t i = 0; i < MESHTASTIC_DELAYED_TX_QUEUE_SIZE; i++) {
        if(mesh_delayed_tx_queue[i].active) {
            count++;
        }
    }
    return count;
}

static uint32_t mesh_delayed_tx_next_ms(uint64_t now_us)
{
    uint64_t best_due = 0;

    for(size_t i = 0; i < MESHTASTIC_DELAYED_TX_QUEUE_SIZE; i++) {
        if(!mesh_delayed_tx_queue[i].active) {
            continue;
        }
        if(best_due == 0ULL || mesh_delayed_tx_queue[i].due_us < best_due) {
            best_due = mesh_delayed_tx_queue[i].due_us;
        }
    }
    if(best_due == 0ULL || best_due <= now_us) {
        return 0U;
    }
    return (uint32_t)((best_due - now_us + 999ULL) / 1000ULL);
}

static bool mesh_delayed_tx_enqueue(const tx_frame_t &frame)
{
    uint64_t now = monotonic_us();
    uint64_t delay = mesh_rebroadcast_delay_us(frame);

    for(size_t i = 0; i < MESHTASTIC_DELAYED_TX_QUEUE_SIZE; i++) {
        if(mesh_delayed_tx_queue[i].active) {
            continue;
        }
        mesh_delayed_tx_queue[i].frame = frame;
        mesh_delayed_tx_queue[i].due_us = now + delay;
        mesh_delayed_tx_queue[i].active = true;
        mesh_rebroadcast_count++;
        daemon_event("Mesh rebroadcast scheduled id=0x%08x hop=%u->%u delay=%lums queued=%u count=%lu",
                     frame.packet_id, frame.old_hop, frame.new_hop,
                     (unsigned long)(delay / 1000ULL),
                     mesh_delayed_tx_count(),
                     (unsigned long)mesh_rebroadcast_count);
        return true;
    }

    mesh_rebroadcast_drop_count++;
    daemon_event("Mesh rebroadcast queue full id=0x%08x drop=%lu",
                 frame.packet_id,
                 (unsigned long)mesh_rebroadcast_drop_count);
    return false;
}

static bool mesh_delayed_ack_enqueue(const tx_frame_t &frame)
{
    uint64_t now = monotonic_us();

    for(size_t i = 0; i < MESHTASTIC_DELAYED_TX_QUEUE_SIZE; i++) {
        if(mesh_delayed_tx_queue[i].active &&
           mesh_delayed_tx_queue[i].frame.routing_ack &&
           mesh_delayed_tx_queue[i].frame.ack_request_id == frame.ack_request_id &&
           mesh_delayed_tx_queue[i].frame.to_node == frame.to_node &&
           mesh_delayed_tx_queue[i].frame.channel == frame.channel) {
            daemon_event("Mesh ACK already queued req=0x%08x to=0x%08x queued=%u",
                         frame.ack_request_id, frame.to_node,
                         mesh_delayed_tx_count());
            return true;
        }
    }

    for(size_t i = 0; i < MESHTASTIC_DELAYED_TX_QUEUE_SIZE; i++) {
        if(mesh_delayed_tx_queue[i].active) {
            continue;
        }
        mesh_delayed_tx_queue[i].frame = frame;
        mesh_delayed_tx_queue[i].due_us =
            now + MESHTASTIC_ACK_RESPONSE_DELAY_US;
        mesh_delayed_tx_queue[i].active = true;
        daemon_event("Mesh ACK scheduled id=0x%08x to=0x%08x delay=%lums queued=%u",
                     frame.packet_id, frame.to_node,
                     (unsigned long)(MESHTASTIC_ACK_RESPONSE_DELAY_US / 1000ULL),
                     mesh_delayed_tx_count());
        return true;
    }

    mesh_ack_drop_count++;
    daemon_event("Mesh ACK queue full id=0x%08x to=0x%08x drop=%lu",
                 frame.packet_id, frame.to_node,
                 (unsigned long)mesh_ack_drop_count);
    return false;
}

static bool mesh_delayed_tx_pop_due(uint64_t now_us, tx_frame_t *frame)
{
    int best = -1;

    if(!frame) {
        return false;
    }
    for(size_t i = 0; i < MESHTASTIC_DELAYED_TX_QUEUE_SIZE; i++) {
        if(!mesh_delayed_tx_queue[i].active ||
           mesh_delayed_tx_queue[i].due_us > now_us) {
            continue;
        }
        if(best < 0 ||
           mesh_delayed_tx_queue[i].due_us <
               mesh_delayed_tx_queue[(size_t)best].due_us) {
            best = (int)i;
        }
    }
    if(best < 0) {
        return false;
    }

    *frame = mesh_delayed_tx_queue[(size_t)best].frame;
    mesh_delayed_tx_queue[(size_t)best].active = false;
    mesh_delayed_tx_queue[(size_t)best].due_us = 0;
    return true;
}

static unsigned int mesh_ack_pending_count(void)
{
    unsigned int count = 0;

    for(size_t i = 0; i < MESHTASTIC_ACK_RETRY_QUEUE_SIZE; i++) {
        if(mesh_ack_retry_queue[i].active) {
            count++;
        }
    }
    return count;
}

static uint32_t mesh_ack_next_ms(uint64_t now_us)
{
    uint64_t best_due = 0;

    for(size_t i = 0; i < MESHTASTIC_ACK_RETRY_QUEUE_SIZE; i++) {
        if(!mesh_ack_retry_queue[i].active) {
            continue;
        }
        if(best_due == 0ULL || mesh_ack_retry_queue[i].due_us < best_due) {
            best_due = mesh_ack_retry_queue[i].due_us;
        }
    }
    if(best_due == 0ULL || best_due <= now_us) {
        return 0U;
    }
    return (uint32_t)((best_due - now_us + 999ULL) / 1000ULL);
}

static uint32_t mesh_nodeinfo_next_ms(uint64_t now_us)
{
    if(mesh_next_nodeinfo_us == 0ULL || mesh_next_nodeinfo_us <= now_us) {
        return 0U;
    }
    return (uint32_t)((mesh_next_nodeinfo_us - now_us + 999ULL) / 1000ULL);
}

static bool mesh_ack_track_frame(const tx_frame_t &frame)
{
    size_t slot = MESHTASTIC_ACK_RETRY_QUEUE_SIZE;

    if(!frame.want_ack || frame.packet_id == 0U ||
       frame.to_node == MESHTASTIC_NODENUM_BROADCAST) {
        return true;
    }

    for(size_t i = 0; i < MESHTASTIC_ACK_RETRY_QUEUE_SIZE; i++) {
        if(mesh_ack_retry_queue[i].active &&
           mesh_ack_retry_queue[i].packet_id == frame.packet_id &&
           mesh_ack_retry_queue[i].to_node == frame.to_node) {
            slot = i;
            break;
        }
        if(slot == MESHTASTIC_ACK_RETRY_QUEUE_SIZE &&
           !mesh_ack_retry_queue[i].active) {
            slot = i;
        }
    }
    if(slot == MESHTASTIC_ACK_RETRY_QUEUE_SIZE) {
        mesh_ack_drop_count++;
        daemon_event("Mesh ACK queue full id=0x%08x to=0x%08x drop=%lu",
                     frame.packet_id, frame.to_node,
                     (unsigned long)mesh_ack_drop_count);
        return false;
    }

    mesh_ack_retry_queue[slot].frame = frame;
    mesh_ack_retry_queue[slot].due_us =
        monotonic_us() + MESHTASTIC_ACK_RETRY_TIMEOUT_US;
    mesh_ack_retry_queue[slot].to_node = frame.to_node;
    mesh_ack_retry_queue[slot].packet_id = frame.packet_id;
    mesh_ack_retry_queue[slot].retries_left = MESHTASTIC_ACK_RETRY_MAX;
    mesh_ack_retry_queue[slot].active = true;
    daemon_event("Mesh ACK wait id=0x%08x to=0x%08x retries=%u pending=%u",
                 frame.packet_id, frame.to_node, MESHTASTIC_ACK_RETRY_MAX,
                 mesh_ack_pending_count());
    return true;
}

static bool mesh_ack_complete(uint32_t from_node, uint32_t packet_id,
                              uint32_t error_reason)
{
    for(size_t i = 0; i < MESHTASTIC_ACK_RETRY_QUEUE_SIZE; i++) {
        if(!mesh_ack_retry_queue[i].active ||
           mesh_ack_retry_queue[i].packet_id != packet_id ||
           mesh_ack_retry_queue[i].to_node != from_node) {
            continue;
        }
        mesh_ack_retry_queue[i].active = false;
        mesh_ack_retry_queue[i].due_us = 0;
        if(error_reason == MESHTASTIC_ROUTING_ERROR_NONE) {
            mesh_ack_rx_count++;
            daemon_event("Mesh ACK received id=0x%08x from=0x%08x ack=%lu pending=%u",
                         packet_id, from_node,
                         (unsigned long)mesh_ack_rx_count,
                         mesh_ack_pending_count());
        } else {
            mesh_nak_rx_count++;
            daemon_event("Mesh NAK received id=0x%08x from=0x%08x err=%u nak=%lu pending=%u",
                         packet_id, from_node, error_reason,
                         (unsigned long)mesh_nak_rx_count,
                         mesh_ack_pending_count());
        }
        return true;
    }
    return false;
}

static bool mesh_should_rebroadcast(const probe_options_t &opts,
                                    const mesh_header_t &header,
                                    bool channel_match)
{
    uint8_t our_relay = (uint8_t)(opts.from_node & 0xffU);

    if(!opts.rebroadcast || !channel_match) {
        return false;
    }
    if(header.from == opts.from_node) {
        return false;
    }
    if(mesh_header_hop_limit(header) == 0U) {
        return false;
    }
    if(header.to == MESHTASTIC_NODENUM_BROADCAST) {
        return true;
    }
    if(header.to == opts.from_node) {
        return false;
    }
    if(header.next_hop != 0U && header.next_hop != our_relay) {
        return false;
    }
    return true;
}

static bool build_mesh_rebroadcast_frame(const probe_options_t &opts,
                                         const uint8_t *data, size_t len,
                                         const mesh_header_t &header,
                                         tx_frame_t *frame)
{
    mesh_header_t fwd = header;
    uint8_t old_hop;
    uint8_t new_hop;
    char summary[220];

    if(!data || !frame || len < MESHTASTIC_HEADER_LENGTH ||
       len > MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        return false;
    }
    old_hop = mesh_header_hop_limit(header);
    if(old_hop == 0U) {
        return false;
    }
    new_hop = (uint8_t)(old_hop - 1U);
    mesh_header_set_hop_limit(&fwd, new_hop);
    fwd.next_hop = 0;
    fwd.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, fwd);
    frame->bytes.insert(frame->bytes.end(), data + MESHTASTIC_HEADER_LENGTH,
                        data + len);
    frame->rebroadcast = true;
    frame->want_ack = false;
    frame->routing_ack = false;
    frame->rebroadcast_from = fwd.from;
    frame->to_node = fwd.to;
    frame->from_node = fwd.from;
    frame->packet_id = fwd.id;
    frame->ack_request_id = 0;
    frame->channel = fwd.channel;
    frame->old_hop = old_hop;
    frame->new_hop = new_hop;
    snprintf(summary, sizeof(summary),
             "mesh rebroadcast id=0x%08x from=0x%08x to=0x%08x ch=0x%02x hop=%u->%u relay=0x%02x len=%u",
             fwd.id, fwd.from, fwd.to, fwd.channel, old_hop, new_hop,
             fwd.relay_node, (unsigned)frame->bytes.size());
    frame->summary = summary;
    return true;
}

static bool build_mesh_frame(const probe_options_t &opts,
                             const std::string &message,
                             tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    mesh_header_t header;
    uint32_t packet_id = opts.packet_id;
    char summary[220];

    if(!frame || !parse_psk(opts.psk, &key)) {
        return false;
    }
    if(packet_id == 0U) {
        packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
        if(packet_id == 0U) {
            packet_id = 1U;
        }
    }
    if(!encode_text_data_proto(message, &data_proto)) {
        return false;
    }
    if(!aes_ctr_crypt(key, opts.from_node, packet_id, &data_proto)) {
        return false;
    }
    if(data_proto.size() + MESHTASTIC_HEADER_LENGTH >
       MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.to = opts.to_node;
    header.from = opts.from_node;
    header.id = packet_id;
    header.flags = (opts.hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                   ((opts.hop_limit << MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT) &
                    MESHTASTIC_PACKET_FLAGS_HOP_START_MASK);
    if(opts.want_ack && header.to != MESHTASTIC_NODENUM_BROADCAST) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    channel_name = effective_mesh_channel_name(opts);
    header.channel = mesh_channel_hash(channel_name, key);
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh id=0x%08x from=0x%08x to=0x%08x ch=0x%02x name=%s hop=%u ack=%s psk=%s text=%s",
             header.id, header.from, header.to, header.channel,
             channel_name.c_str(), opts.hop_limit,
             frame->want_ack ? "on" : "off",
             key.empty() ? "none" : opts.psk.c_str(), message.c_str());
    frame->summary = summary;
    return true;
}

static bool build_phoneapi_mesh_data_frame(const probe_options_t &opts,
                                           const phoneapi_mesh_tx_t &tx,
                                           tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    mesh_header_t header;
    uint32_t packet_id = tx.packet_id;
    uint8_t hop_limit = tx.hop_limit != 0U ? tx.hop_limit : opts.hop_limit;
    char summary[220];

    if(!frame || !parse_psk(opts.psk, &key)) {
        return false;
    }
    if(packet_id == 0U) {
        packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
        if(packet_id == 0U) {
            packet_id = 1U;
        }
    }
    if(!encode_data_proto(tx.data.portnum, tx.data.payload,
                          tx.data.request_id, tx.data.reply_id,
                          &data_proto)) {
        return false;
    }
    if(!aes_ctr_crypt(key, opts.from_node, packet_id, &data_proto)) {
        return false;
    }
    if(data_proto.size() + MESHTASTIC_HEADER_LENGTH >
       MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.to = tx.to_node == 0U ? MESHTASTIC_NODENUM_BROADCAST : tx.to_node;
    header.from = opts.from_node;
    header.id = packet_id;
    header.flags = (hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                   ((hop_limit << MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT) &
                    MESHTASTIC_PACKET_FLAGS_HOP_START_MASK);
    if(tx.want_ack && header.to != MESHTASTIC_NODENUM_BROADCAST) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    channel_name = effective_mesh_channel_name(opts);
    header.channel = mesh_channel_hash(channel_name, key);
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "phoneapi mesh id=0x%08x from=0x%08x to=0x%08x ch=0x%02x port=%u payload=%u hop=%u ack=%s",
             header.id, header.from, header.to, header.channel,
             tx.data.portnum, (unsigned)tx.data.payload.size(), hop_limit,
             frame->want_ack ? "on" : "off");
    frame->summary = summary;
    return true;
}

static bool build_mesh_nodeinfo_frame(const probe_options_t &opts,
                                      tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> user_proto;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    mesh_header_t header;
    uint32_t packet_id;
    char summary[220];

    if(!frame || !parse_psk(opts.psk, &key)) {
        return false;
    }
    packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
    if(packet_id == 0U) {
        packet_id = 1U;
    }
    if(!encode_user_proto(opts, &user_proto) ||
       !encode_data_proto(MESHTASTIC_NODEINFO_APP, user_proto, 0, 0,
                          &data_proto)) {
        return false;
    }
    if(!aes_ctr_crypt(key, opts.from_node, packet_id, &data_proto)) {
        return false;
    }
    if(data_proto.size() + MESHTASTIC_HEADER_LENGTH >
       MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.to = MESHTASTIC_NODENUM_BROADCAST;
    header.from = opts.from_node;
    header.id = packet_id;
    header.flags = (opts.hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                   ((opts.hop_limit << MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT) &
                    MESHTASTIC_PACKET_FLAGS_HOP_START_MASK);
    channel_name = effective_mesh_channel_name(opts);
    header.channel = mesh_channel_hash(channel_name, key);
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = false;
    frame->routing_ack = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh nodeinfo id=0x%08x from=0x%08x ch=0x%02x name=%s hop=%u psk=%s",
             header.id, header.from, header.channel, opts.node_name.c_str(),
             opts.hop_limit, key.empty() ? "none" : opts.psk.c_str());
    frame->summary = summary;
    return true;
}

static bool build_mesh_ack_frame(const probe_options_t &opts,
                                 const mesh_header_t &rx_header,
                                 uint32_t error_reason,
                                 bool ack_wants_ack,
                                 tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> routing_proto;
    std::vector<uint8_t> data_proto;
    mesh_header_t header;
    uint32_t packet_id;
    uint8_t ack_hop;
    char summary[220];

    if(!frame || !parse_psk(opts.psk, &key)) {
        return false;
    }
    if(!encode_routing_proto(error_reason, &routing_proto) ||
       !encode_data_proto(MESHTASTIC_ROUTING_APP, routing_proto,
                          rx_header.id, 0, &data_proto)) {
        return false;
    }
    packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^
                mesh_prng_u32(rx_header.id);
    if(packet_id == 0U) {
        packet_id = 1U;
    }
    if(!aes_ctr_crypt(key, opts.from_node, packet_id, &data_proto)) {
        return false;
    }
    if(data_proto.size() + MESHTASTIC_HEADER_LENGTH >
       MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.to = rx_header.from;
    header.from = opts.from_node;
    header.id = packet_id;
    ack_hop = opts.hop_limit > 7U ? 7U : opts.hop_limit;
    header.flags = (ack_hop & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                   ((ack_hop << MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT) &
                    MESHTASTIC_PACKET_FLAGS_HOP_START_MASK);
    if(ack_wants_ack) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    header.channel = rx_header.channel;
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = true;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = rx_header.id;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh ack id=0x%08x req=0x%08x from=0x%08x to=0x%08x ch=0x%02x hop=%u err=%u ack=%s",
             header.id, rx_header.id, header.from, header.to, header.channel,
             ack_hop, error_reason, frame->want_ack ? "on" : "off");
    frame->summary = summary;
    return true;
}

static tx_frame_t build_raw_frame(const probe_options_t &opts,
                                  const std::string &message)
{
    char header[96];
    tx_frame_t frame;

    snprintf(header, sizeof(header), "K230PROBE1|%lu|%s|",
             (unsigned long)++seq_count, opts.node_name.c_str());
    frame.summary = header;
    frame.summary += message;
    if(frame.summary.size() > 240U) {
        frame.summary.resize(240U);
    }
    frame.bytes.assign(frame.summary.begin(), frame.summary.end());
    return frame;
}

static bool build_tx_frame(const probe_options_t &opts,
                           const std::string &message, tx_frame_t *frame)
{
    if(opts.mesh_mode) {
        return build_mesh_frame(opts, message, frame);
    }
    if(!frame) {
        return false;
    }
    *frame = build_raw_frame(opts, message);
    return true;
}

static bool process_mesh_rx(const probe_options_t &opts, const uint8_t *data,
                            size_t len, float rssi, float snr,
                            tx_frame_t *rebroadcast_frame)
{
    mesh_header_t header;
    std::vector<uint8_t> key;
    std::vector<uint8_t> payload;
    std::string channel_name;
    mesh_data_proto_t decoded;
    uint8_t hop_limit;
    uint8_t hop_start;
    bool psk_ok;
    bool channel_match = false;
    bool duplicate = false;
    bool should_rebroadcast = false;
    bool data_ok = false;
    bool ack_candidate = false;
    bool ack_wants_ack = false;

    if(!parse_mesh_header(data, len, &header)) {
        daemon_event("RX %lu len=%u rssi=%.1f snr=%.1f mesh=short",
                     (unsigned long)rx_count, (unsigned)len, rssi, snr);
        return false;
    }
    hop_limit = mesh_header_hop_limit(header);
    hop_start = mesh_header_hop_start(header);
    if(header.from != opts.from_node) {
        mesh_node_seen(header.from, rssi, snr);
    }
    duplicate = mesh_history_contains(header);
    if(duplicate) {
        mesh_duplicate_count++;
    } else {
        mesh_history_remember(header);
    }

    payload.assign(data + MESHTASTIC_HEADER_LENGTH, data + len);
    channel_name = effective_mesh_channel_name(opts);
    psk_ok = parse_psk(opts.psk, &key);
    if(psk_ok && header.channel == mesh_channel_hash(channel_name, key)) {
        channel_match = true;
        (void)aes_ctr_crypt(key, header.from, header.id, &payload);
    }
    data_ok = decode_data_proto(payload.data(), payload.size(), &decoded);
    if(data_ok) {
        if(channel_match && !duplicate && header.from != opts.from_node) {
            phoneapi_notify_mesh_rx(header, payload, rssi, snr);
        }
        if(decoded.portnum == MESHTASTIC_TEXT_MESSAGE_APP) {
            std::string text;
            std::string clean = mesh_clean_text(text);

            if(!decoded.payload.empty()) {
                text.assign((const char *)decoded.payload.data(),
                            decoded.payload.size());
                clean = mesh_clean_text(text);
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u ack=%s rssi=%.1f snr=%.1f port=%u text=%s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         mesh_header_want_ack(header) ? "yes" : "no",
                         rssi, snr, decoded.portnum, clean.c_str(),
                         duplicate ? " duplicate" : "");
            if(!duplicate && channel_match && !clean.empty() &&
               (header.to == MESHTASTIC_NODENUM_BROADCAST ||
                header.to == opts.from_node || header.from == opts.from_node)) {
                if(mesh_chat_text_seen_recently(header.from, header.to, clean)) {
                    daemon_event("Mesh chat duplicate text suppressed from=0x%08x to=0x%08x text=%s",
                                 header.from, header.to, clean.c_str());
                } else {
                    daemon_chat("RX 0x%08x rssi=%ddBm snr=%.1f: %s",
                                header.from, (int)roundf(rssi), snr,
                                clean.c_str());
                }
            }
        } else if(decoded.portnum == MESHTASTIC_POSITION_APP) {
            mesh_position_info_t position;
            bool position_ok = decode_position_proto(decoded.payload,
                                                     &position);
            char alt_text[24];
            char speed_text[24];
            char track_text[24];

            if(position_ok && channel_match) {
                mesh_node_update_position(header.from, position);
            }
            if(position_ok && position.has_altitude) {
                snprintf(alt_text, sizeof(alt_text), "%dm",
                         position.altitude_m);
            } else {
                snprintf(alt_text, sizeof(alt_text), "-");
            }
            if(position_ok && position.has_ground_speed) {
                snprintf(speed_text, sizeof(speed_text), "%.2fm/s",
                         position.ground_speed_cms / 100.0);
            } else {
                snprintf(speed_text, sizeof(speed_text), "-");
            }
            if(position_ok && position.has_ground_track) {
                snprintf(track_text, sizeof(track_text), "%.1fdeg",
                         position.ground_track_1e5 / 100000.0);
            } else {
                snprintf(track_text, sizeof(track_text), "-");
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u pos=%s lat=%.7f lon=%.7f alt=%s speed=%s track=%s sats=%u precision=%u%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         position_ok ? "ok" : "decode-failed",
                         position.latitude_i * 1e-7,
                         position.longitude_i * 1e-7,
                         alt_text, speed_text, track_text,
                         position.sats_in_view, position.precision_bits,
                         duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_NODEINFO_APP) {
            mesh_user_info_t user;
            bool user_ok = decode_user_proto(decoded.payload, &user);
            std::string clean_long = mesh_clean_text(user.long_name);
            std::string clean_short = mesh_clean_text(user.short_name);

            if(channel_match && user_ok) {
                mesh_node_update_user(header.from, user);
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u nodeinfo=%s long=%s short=%s hw=%d%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         user_ok ? "ok" : "decode-failed",
                         clean_long.empty() ? "-" : clean_long.c_str(),
                         clean_short.empty() ? "-" : clean_short.c_str(),
                         user.hw_model, duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_TELEMETRY_APP) {
            mesh_telemetry_info_t telemetry;
            bool telemetry_ok = decode_telemetry_proto(decoded.payload,
                                                       &telemetry);
            std::string summary = telemetry_summary(telemetry);

            if(telemetry_ok && channel_match) {
                mesh_node_update_telemetry(header.from, telemetry);
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u telemetry=%s %s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         telemetry_ok ? "ok" : "decode-failed",
                         summary.c_str(), duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_NEIGHBORINFO_APP) {
            mesh_neighbor_info_t neighbor_info;
            bool neighbor_ok = decode_neighbor_info_proto(decoded.payload,
                                                          &neighbor_info);

            if(neighbor_ok && channel_match) {
                mesh_node_update_neighbor_info(header.from, neighbor_info);
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u neighbor=%s owner=0x%08x last=0x%08x count=%u list=%s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         neighbor_ok ? "ok" : "decode-failed",
                         neighbor_info.node_id,
                         neighbor_info.last_sent_by_id,
                         neighbor_info.neighbor_count,
                         neighbor_info.summary.empty() ? "-" :
                             neighbor_info.summary.c_str(),
                         duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_ROUTING_APP) {
            uint32_t error_reason = MESHTASTIC_ROUTING_ERROR_NONE;
            bool routing_ok = decode_routing_error_proto(decoded.payload,
                                                         &error_reason);
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u request=0x%08x routing=%s err=%u%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum, decoded.request_id,
                         routing_ok ? "ok" : "decode-failed",
                         error_reason, duplicate ? " duplicate" : "");
            if(channel_match && routing_ok && header.to == opts.from_node &&
               decoded.request_id != 0U) {
                if(!mesh_ack_complete(header.from, decoded.request_id,
                                      error_reason)) {
                    daemon_event("Mesh ACK no pending id=0x%08x from=0x%08x err=%u",
                                 decoded.request_id, header.from,
                                 error_reason);
                }
            }
        } else {
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u payload_len=%u%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         (unsigned)decoded.payload.size(),
                         duplicate ? " duplicate" : "");
        }
    } else {
        daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f payload_len=%u decode=failed%s",
                     (unsigned long)rx_count, header.from, header.to,
                     header.id, header.channel, hop_limit, hop_start, rssi,
                     snr, (unsigned)payload.size(),
                     duplicate ? " duplicate" : "");
    }

    ack_candidate = channel_match && data_ok && header.to == opts.from_node &&
                    header.from != opts.from_node &&
                    mesh_header_want_ack(header);
    if(ack_candidate) {
        ack_wants_ack = decoded.portnum == MESHTASTIC_TEXT_MESSAGE_APP &&
                        decoded.request_id == 0U &&
                        decoded.reply_id == 0U;
    }
    if(ack_candidate &&
       build_mesh_ack_frame(opts, header, MESHTASTIC_ROUTING_ERROR_NONE,
                            ack_wants_ack, rebroadcast_frame)) {
        daemon_event("Mesh ACK candidate req=0x%08x to=0x%08x ack=%s",
                     header.id, header.from,
                     ack_wants_ack ? "reliable" : "plain");
        if(duplicate) {
            daemon_event("Mesh ACK duplicate request id=0x%08x from=0x%08x",
                         header.id, header.from);
            return true;
        }
    } else {
        ack_candidate = false;
    }

    if(duplicate) {
        daemon_event("Mesh drop duplicate from=0x%08x id=0x%08x ch=0x%02x dup=%lu",
                     header.from, header.id, header.channel,
                     (unsigned long)mesh_duplicate_count);
        return false;
    }
    if(ack_candidate) {
        return true;
    }
    should_rebroadcast = mesh_should_rebroadcast(opts, header, channel_match);
    if(!should_rebroadcast) {
        if(opts.rebroadcast && header.from != opts.from_node &&
           header.to == MESHTASTIC_NODENUM_BROADCAST && !channel_match) {
            mesh_rebroadcast_drop_count++;
            daemon_event("Mesh rebroadcast skip id=0x%08x reason=channel-mismatch drop=%lu",
                         header.id,
                         (unsigned long)mesh_rebroadcast_drop_count);
        }
        return false;
    }
    if(build_mesh_rebroadcast_frame(opts, data, len, header,
                                    rebroadcast_frame)) {
        daemon_event("Mesh rebroadcast candidate id=0x%08x hop=%u->%u",
                     header.id, hop_limit, hop_limit - 1U);
        return true;
    }
    mesh_rebroadcast_drop_count++;
    daemon_event("Mesh rebroadcast build failed id=0x%08x drop=%lu",
                 header.id, (unsigned long)mesh_rebroadcast_drop_count);
    return false;
}

static bool fd_write_all(int fd, const char *data, size_t len)
{
    while(len > 0U) {
        ssize_t rc = send(fd, data, len, MSG_NOSIGNAL);
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        if(rc == 0) {
            return false;
        }
        data += rc;
        len -= (size_t)rc;
    }
    return true;
}

static std::string trim_ipc_line(const char *data)
{
    std::string line = data ? data : "";
    size_t start = 0;
    size_t end = line.size();

    while(start < end && isspace((unsigned char)line[start])) {
        start++;
    }
    while(end > start && isspace((unsigned char)line[end - 1U])) {
        end--;
    }
    return line.substr(start, end - start);
}

static int set_fd_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if(flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int setup_daemon_socket(const std::string &path)
{
    struct sockaddr_un addr;
    int fd;

    if(path.empty() || path.size() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Daemon socket path invalid: %s\n", path.c_str());
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
        fprintf(stderr, "socket(AF_UNIX) failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    unlink(path.c_str());

    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind(%s) failed: %s\n", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    if(listen(fd, 4) != 0) {
        fprintf(stderr, "listen(%s) failed: %s\n", path.c_str(), strerror(errno));
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    if(set_fd_nonblock(fd) != 0) {
        fprintf(stderr, "nonblock(%s) failed: %s\n", path.c_str(), strerror(errno));
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    chmod(path.c_str(), 0666);
    return fd;
}

static std::string daemon_status_response(const probe_options_t &opts,
                                          chip_type_t chip,
                                          const std::string &pending_send)
{
    char buf[1280];
    char ble_detail[160];
    phoneapi_bridge_state_t ble_state;
    const char *queued = pending_send.empty() ? "0" : "1";
    uint64_t now = monotonic_us();

    ble_state = phoneapi_bridge_get_state(ble_detail, sizeof(ble_detail));
    snprintf(buf, sizeof(buf),
             "OK version=%s chip=%s op=%s tx=%lu rx=%lu queued=%s "
             "ble=%s ble_detail=%s "
             "hist=%u dup=%lu rebroadcast=%lu rebroadcast_drop=%lu "
             "delayed=%u next_rebroadcast_ms=%u "
             "ack_pending=%u ack_next_ms=%u ack_rx=%lu nak_rx=%lu "
             "ack_retry=%lu ack_timeout=%lu ack_drop=%lu "
             "nodeinfo_tx=%lu nodeinfo_drop=%lu next_nodeinfo_ms=%u "
             "region=%s preset=%s freq=%.3f bw=%.1f sf=%u cr=4/%u sw=0x%02x power=%d node=%s "
             "from=0x%08x to=0x%08x want_ack=%s relay=%s channel=%s socket=%s\n",
             PROBE_VERSION, chip_name(chip), op_name(active_op),
             (unsigned long)tx_count, (unsigned long)rx_count, queued,
             phoneapi_bridge_state_name(ble_state), ble_detail,
             (unsigned)mesh_history_count,
             (unsigned long)mesh_duplicate_count,
             (unsigned long)mesh_rebroadcast_count,
             (unsigned long)mesh_rebroadcast_drop_count,
             mesh_delayed_tx_count(),
             mesh_delayed_tx_next_ms(now),
             mesh_ack_pending_count(),
             mesh_ack_next_ms(now),
             (unsigned long)mesh_ack_rx_count,
             (unsigned long)mesh_nak_rx_count,
             (unsigned long)mesh_ack_retry_count,
             (unsigned long)mesh_ack_timeout_count,
             (unsigned long)mesh_ack_drop_count,
             (unsigned long)mesh_nodeinfo_tx_count,
             (unsigned long)mesh_nodeinfo_drop_count,
             mesh_nodeinfo_next_ms(now),
             opts.resolved_region.empty() ? "-" : opts.resolved_region.c_str(),
             opts.resolved_preset.empty() ? "-" : opts.resolved_preset.c_str(),
             opts.profile.freq, opts.profile.bandwidth, opts.profile.sf,
             opts.profile.cr, opts.profile.sync_word, opts.profile.power,
             opts.node_name.c_str(), opts.from_node,
             opts.to_node, opts.want_ack ? "on" : "off",
             opts.rebroadcast ? "on" : "off",
             effective_mesh_channel_name(opts).c_str(),
             opts.socket_path.c_str());
    return std::string(buf);
}

static std::string daemon_event_log_response(void)
{
    std::string response = "OK log\n";

    pthread_mutex_lock(&daemon_log_mutex);
    if(daemon_event_log_count == 0U) {
        response += "No daemon events yet\n";
        pthread_mutex_unlock(&daemon_log_mutex);
        return response;
    }
    for(size_t i = 0; i < daemon_event_log_count; i++) {
        response += daemon_event_log[i];
        response += "\n";
    }
    pthread_mutex_unlock(&daemon_log_mutex);
    return response;
}

static std::string daemon_chat_log_response(void)
{
    std::string response = "OK chat\n";

    pthread_mutex_lock(&daemon_log_mutex);
    if(daemon_chat_log_count == 0U) {
        response += "No mesh messages yet\n";
        pthread_mutex_unlock(&daemon_log_mutex);
        return response;
    }
    for(size_t i = 0; i < daemon_chat_log_count; i++) {
        response += daemon_chat_log[i];
        response += "\n";
    }
    pthread_mutex_unlock(&daemon_log_mutex);
    return response;
}

static std::string daemon_nodes_response(void)
{
    char line[768];
    uint64_t now = monotonic_us();
    std::string response = "OK nodes\n";

    if(mesh_node_count == 0U) {
        response += "No nodes seen yet\n";
        return response;
    }
    snprintf(line, sizeof(line), "Node count: %u\n",
             (unsigned)mesh_node_count);
    response += line;
    for(size_t i = 0; i < mesh_node_count; i++) {
        uint32_t age_s = 0;
        const char *long_name = mesh_nodes[i].long_name[0] ?
                                mesh_nodes[i].long_name : "-";
        const char *short_name = mesh_nodes[i].short_name[0] ?
                                 mesh_nodes[i].short_name : "-";
        std::string telemetry = telemetry_summary(mesh_nodes[i]);
        const char *neighbor = mesh_nodes[i].has_neighbor_info ?
                               mesh_nodes[i].neighbor_summary : "-";
        if(mesh_nodes[i].last_seen_us <= now) {
            age_s = (uint32_t)((now - mesh_nodes[i].last_seen_us) / 1000000ULL);
        }
        if(mesh_nodes[i].has_position) {
            char alt_text[24];
            char speed_text[24];
            char track_text[24];

            if(mesh_nodes[i].has_altitude) {
                snprintf(alt_text, sizeof(alt_text), "%dm",
                         mesh_nodes[i].altitude_m);
            } else {
                snprintf(alt_text, sizeof(alt_text), "-");
            }
            if(mesh_nodes[i].has_ground_speed) {
                snprintf(speed_text, sizeof(speed_text), "%.2fm/s",
                         mesh_nodes[i].ground_speed_cms / 100.0);
            } else {
                snprintf(speed_text, sizeof(speed_text), "-");
            }
            if(mesh_nodes[i].has_ground_track) {
                snprintf(track_text, sizeof(track_text), "%.1fdeg",
                         mesh_nodes[i].ground_track_1e5 / 100000.0);
            } else {
                snprintf(track_text, sizeof(track_text), "-");
            }
            snprintf(line, sizeof(line),
                     "0x%08x name=%s short=%s hw=%d rx=%lu age=%us rssi=%ddBm snr=%.1f pos=%.7f,%.7f alt=%s speed=%s track=%s sats=%u precision=%u time=%u tel=%s nbr=%s\n",
                     mesh_nodes[i].node, long_name, short_name,
                     mesh_nodes[i].hw_model,
                     (unsigned long)mesh_nodes[i].rx_count, age_s,
                     mesh_nodes[i].rssi_dbm, mesh_nodes[i].snr,
                     mesh_nodes[i].latitude_i * 1e-7,
                     mesh_nodes[i].longitude_i * 1e-7,
                     alt_text, speed_text, track_text,
                     mesh_nodes[i].sats_in_view,
                     mesh_nodes[i].precision_bits,
                     mesh_nodes[i].position_timestamp, telemetry.c_str(),
                     neighbor);
        } else {
            snprintf(line, sizeof(line),
                     "0x%08x name=%s short=%s hw=%d rx=%lu age=%us rssi=%ddBm snr=%.1f pos=- tel=%s nbr=%s\n",
                     mesh_nodes[i].node, long_name, short_name,
                     mesh_nodes[i].hw_model,
                     (unsigned long)mesh_nodes[i].rx_count, age_s,
                     mesh_nodes[i].rssi_dbm, mesh_nodes[i].snr,
                     telemetry.c_str(), neighbor);
        }
        response += line;
    }
    return response;
}

static std::string handle_daemon_command(const std::string &line,
                                         const probe_options_t &opts,
                                         chip_type_t chip,
                                         std::string *pending_send)
{
    std::string message;

    if(line == "STATUS" || line == "status") {
        return daemon_status_response(opts, chip, pending_send ? *pending_send :
                                      std::string());
    }
    if(line == "LOG" || line == "log") {
        return daemon_event_log_response();
    }
    if(line == "CHAT" || line == "chat") {
        return daemon_chat_log_response();
    }
    if(line == "NODES" || line == "nodes") {
        return daemon_nodes_response();
    }
    if(line == "QUIT" || line == "quit") {
        running = 0;
        return "OK quitting\n";
    }
    if(line.compare(0, 5, "SEND ") == 0 || line.compare(0, 5, "send ") == 0) {
        if(!pending_send) {
            return "ERR internal\n";
        }
        message = trim_ipc_line(line.c_str() + 5);
        if(message.empty()) {
            return "ERR empty-message\n";
        }
        if(message.size() > MESHTASTIC_MAX_IPC_MESSAGE_LEN) {
            return "ERR message-too-long\n";
        }
        if(active_op == OP_TX || !pending_send->empty()) {
            return "ERR busy\n";
        }
        *pending_send = message;
        char buf[96];
        snprintf(buf, sizeof(buf), "OK queued len=%u\n",
                 (unsigned)message.size());
        return std::string(buf);
    }
    return "ERR unknown-command\n";
}

static void accept_daemon_clients(int server_fd, const probe_options_t &opts,
                                  chip_type_t chip,
                                  std::string *pending_send)
{
    for(;;) {
        struct pollfd pfd;
        char buf[512];
        int client_fd = accept(server_fd, nullptr, nullptr);
        ssize_t n;
        std::string line;
        std::string response;

        if(client_fd < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            fprintf(stderr, "daemon accept failed: %s\n", strerror(errno));
            return;
        }

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        if(poll(&pfd, 1, 50) <= 0 || !(pfd.revents & POLLIN)) {
            response = "ERR no-command\n";
        } else {
            memset(buf, 0, sizeof(buf));
            n = recv(client_fd, buf, sizeof(buf) - 1U, 0);
            if(n <= 0) {
                response = "ERR read-failed\n";
            } else {
                buf[n] = '\0';
                line = trim_ipc_line(buf);
                response = handle_daemon_command(line, opts, chip,
                                                 pending_send);
            }
        }
        (void)fd_write_all(client_fd, response.c_str(), response.size());
        close(client_fd);
    }
}

static int run_daemon_client(const probe_options_t &opts)
{
    struct sockaddr_un addr;
    std::string command;
    char buf[512];
    int fd;
    bool wrote_response = false;

    if(opts.client_status) {
        command = "STATUS\n";
    } else if(opts.client_log) {
        command = "LOG\n";
    } else if(opts.client_chat) {
        command = "CHAT\n";
    } else if(opts.client_nodes) {
        command = "NODES\n";
    } else if(opts.client_quit) {
        command = "QUIT\n";
    } else if(opts.client_send_requested) {
        if(opts.client_send.empty()) {
            fprintf(stderr, "--cmd-send message is empty\n");
            return 2;
        }
        command = "SEND " + opts.client_send + "\n";
    } else {
        fprintf(stderr, "No daemon client command requested\n");
        return 2;
    }

    if(opts.socket_path.empty() || opts.socket_path.size() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Daemon socket path invalid: %s\n", opts.socket_path.c_str());
        return 2;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
        fprintf(stderr, "socket(AF_UNIX) failed: %s\n", strerror(errno));
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", opts.socket_path.c_str());
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "connect(%s) failed: %s\n",
                opts.socket_path.c_str(), strerror(errno));
        close(fd);
        return 1;
    }
    if(!fd_write_all(fd, command.c_str(), command.size())) {
        fprintf(stderr, "send command failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    shutdown(fd, SHUT_WR);
    for(;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            fprintf(stderr, "read response failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        if(n == 0) {
            break;
        }
        fwrite(buf, 1, (size_t)n, stdout);
        wrote_response = true;
    }
    if(!wrote_response) {
        printf("ERR no-response\n");
    }
    close(fd);
    return 0;
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "k230_meshtastic_probe " PROBE_VERSION "\n"
            "Simple LoRa mesh probe for K230 T-Display.\n\n"
            "Usage:\n"
            "  %s --listen [profile options]\n"
            "  %s --send \"hello\" [profile options]\n"
            "  %s --auto --message \"ping\" --interval 1000 [profile options]\n"
            "  %s --daemon [profile options]\n"
            "  %s --cmd-status|--cmd-log|--cmd-chat|--cmd-nodes|--cmd-send \"hello\"|--cmd-quit [--socket PATH]\n\n"
            "Daemon options:\n"
            "  --daemon        Run as local Meshtastic socket daemon, implies --mesh\n"
            "  --socket PATH   Default " MESHTASTIC_DEFAULT_SOCKET_PATH "\n"
            "  --cmd-status    Query running daemon status and exit\n"
            "  --cmd-log       Query recent daemon TX/RX event log and exit\n"
            "  --cmd-chat      Query recent decoded text messages and exit\n"
            "  --cmd-nodes     Query recently seen mesh nodes and exit\n"
            "  --cmd-send MSG  Ask running daemon to transmit MSG and exit\n"
            "  --cmd-quit      Ask running daemon to exit\n\n"
            "Profile options:\n"
            "  --region NAME    Meshtastic region, default US when --mesh is used\n"
            "  --preset NAME    Meshtastic preset, default region preset\n"
            "  --slot N         1-based Meshtastic frequency slot override\n"
            "  --freq MHz       Raw default 915.0, overrides calculated profile\n"
            "  --bw kHz         Raw default 125.0, overrides calculated profile\n"
            "  --sf N           Raw default 12, overrides calculated profile\n"
            "  --cr N           Default 5 for CR 4/5\n"
            "  --sw VALUE       Raw default 0xCD, Meshtastic default 0x2B\n"
            "  --power dBm      Default 22\n"
            "  --preamble N     Default 16\n"
            "  --node NAME      Default hostname-pid\n"
            "  --duration SEC   Exit after SEC seconds, 0 means forever\n\n"
            "Meshtastic frame options:\n"
            "  --mesh           Use 16-byte Meshtastic radio header + Data protobuf\n"
            "  --from NODE      Sender node number, default is derived from --node\n"
            "  --to NODE        Destination, default 0xffffffff broadcast\n"
            "  --packet-id ID   Fixed packet ID, default auto\n"
            "  --hop-limit N    Default 3\n"
            "  --ack            Request Routing ACK for direct messages\n"
            "  --no-ack         Disable Routing ACK request\n"
            "  --nodeinfo       Send one NodeInfo packet when daemon starts (default)\n"
            "  --no-nodeinfo    Do not advertise this node on daemon start\n"
            "  --nodeinfo-interval SEC  Periodic daemon NodeInfo interval, default 600\n"
            "  --no-rebroadcast Disable minimal broadcast flood forwarding\n"
            "  --channel-name S Default primary channel name, empty uses preset name\n"
            "  --psk VALUE      default, none/off/0, or 16/32-byte hex key\n",
            argv0, argv0, argv0, argv0, argv0);
}

static bool parse_u32(const char *text, uint32_t *out, int base)
{
    char *endp = nullptr;
    unsigned long value;

    if(!text || !out) {
        return false;
    }
    errno = 0;
    value = strtoul(text, &endp, base);
    if(errno != 0 || endp == text || *endp != '\0' || value > 0xffffffffUL) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_options(int argc, char **argv, probe_options_t *opts)
{
    uint32_t tmp;

    if(!opts) {
        return false;
    }
    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if(strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if(strcmp(arg, "--listen") == 0) {
            continue;
        } else if(strcmp(arg, "--auto") == 0) {
            opts->auto_tx = true;
        } else if(strcmp(arg, "--mesh") == 0) {
            opts->mesh_mode = true;
        } else if(strcmp(arg, "--no-rebroadcast") == 0) {
            opts->rebroadcast = false;
        } else if(strcmp(arg, "--nodeinfo") == 0) {
            opts->advertise_nodeinfo = true;
        } else if(strcmp(arg, "--no-nodeinfo") == 0) {
            opts->advertise_nodeinfo = false;
        } else if(strcmp(arg, "--nodeinfo-interval") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10)) {
            if(tmp < 60U) {
                tmp = 60U;
            }
            opts->nodeinfo_interval_sec = tmp;
        } else if(strcmp(arg, "--ack") == 0) {
            opts->want_ack = true;
            opts->want_ack_set = true;
        } else if(strcmp(arg, "--no-ack") == 0) {
            opts->want_ack = false;
            opts->want_ack_set = true;
        } else if(strcmp(arg, "--daemon") == 0) {
            opts->daemon_mode = true;
            opts->mesh_mode = true;
        } else if(strcmp(arg, "--socket") == 0 && i + 1 < argc) {
            opts->socket_path = argv[++i];
        } else if(strcmp(arg, "--cmd-status") == 0) {
            opts->client_status = true;
        } else if(strcmp(arg, "--cmd-log") == 0) {
            opts->client_log = true;
        } else if(strcmp(arg, "--cmd-chat") == 0) {
            opts->client_chat = true;
        } else if(strcmp(arg, "--cmd-nodes") == 0) {
            opts->client_nodes = true;
        } else if(strcmp(arg, "--cmd-quit") == 0) {
            opts->client_quit = true;
        } else if(strcmp(arg, "--cmd-send") == 0 && i + 1 < argc) {
            opts->client_send = argv[++i];
            opts->client_send_requested = true;
        } else if(strcmp(arg, "--send") == 0 && i + 1 < argc) {
            opts->send_once = argv[++i];
        } else if(strcmp(arg, "--message") == 0 && i + 1 < argc) {
            opts->message = argv[++i];
        } else if(strcmp(arg, "--node") == 0 && i + 1 < argc) {
            opts->node_name = argv[++i];
        } else if(strcmp(arg, "--channel-name") == 0 && i + 1 < argc) {
            opts->channel_name = argv[++i];
        } else if(strcmp(arg, "--psk") == 0 && i + 1 < argc) {
            opts->psk = argv[++i];
        } else if(strcmp(arg, "--spi") == 0 && i + 1 < argc) {
            opts->spi_path = argv[++i];
        } else if(strcmp(arg, "--region") == 0 && i + 1 < argc) {
            opts->region = argv[++i];
        } else if(strcmp(arg, "--preset") == 0 && i + 1 < argc) {
            opts->preset = argv[++i];
        } else if((strcmp(arg, "--slot") == 0 ||
                   strcmp(arg, "--channel-num") == 0) && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10)) {
            opts->frequency_slot = tmp;
        } else if(strcmp(arg, "--from") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 0)) {
            opts->from_node = tmp;
        } else if(strcmp(arg, "--to") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 0)) {
            opts->to_node = tmp;
        } else if(strcmp(arg, "--packet-id") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 0)) {
            opts->packet_id = tmp;
        } else if(strcmp(arg, "--hop-limit") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10) && tmp <= 7U) {
            opts->hop_limit = (uint8_t)tmp;
        } else if(strcmp(arg, "--freq") == 0 && i + 1 < argc) {
            opts->profile.freq = strtof(argv[++i], nullptr);
            opts->manual_freq = true;
        } else if(strcmp(arg, "--bw") == 0 && i + 1 < argc) {
            opts->profile.bandwidth = strtof(argv[++i], nullptr);
            opts->manual_bw = true;
        } else if(strcmp(arg, "--sf") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10) && tmp <= 255U) {
            opts->profile.sf = (uint8_t)tmp;
            opts->manual_sf = true;
        } else if(strcmp(arg, "--cr") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10) && tmp <= 255U) {
            opts->profile.cr = (uint8_t)tmp;
            opts->manual_cr = true;
        } else if(strcmp(arg, "--sw") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 0) && tmp <= 255U) {
            opts->profile.sync_word = (uint8_t)tmp;
            opts->manual_sw = true;
        } else if(strcmp(arg, "--power") == 0 && i + 1 < argc) {
            opts->profile.power = (int8_t)strtol(argv[++i], nullptr, 10);
            opts->manual_power = true;
        } else if(strcmp(arg, "--preamble") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10) && tmp <= 65535U) {
            opts->profile.preamble = (uint16_t)tmp;
            opts->manual_preamble = true;
        } else if(strcmp(arg, "--interval") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10)) {
            opts->interval_ms = tmp < 100U ? 100U : tmp;
        } else if(strcmp(arg, "--duration") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10)) {
            opts->duration_sec = tmp;
        } else {
            fprintf(stderr, "Unknown or invalid option: %s\n", arg);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

static bool default_node_name_from_netdev(const char *ifname, std::string *name)
{
    char path[96];
    char line[96];
    char compact[13];
    FILE *fp;
    size_t out = 0;

    if(!ifname || !name) {
        return false;
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    fp = fopen(path, "r");
    if(!fp) {
        return false;
    }
    if(!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    for(size_t i = 0; line[i] && out < sizeof(compact) - 1U; i++) {
        if(isxdigit((unsigned char)line[i])) {
            compact[out++] = (char)tolower((unsigned char)line[i]);
        }
    }
    compact[out] = '\0';
    if(out < 4U) {
        return false;
    }
    *name = std::string("k230-") + (compact + out - 4U);
    return true;
}

static void default_node_name(std::string *name)
{
    char host[64] = "k230";
    char buf[96];

    if(!name) {
        return;
    }
    if(!name->empty() && *name != "k230-t-display" &&
       *name != "nRF52840" && *name != "K230 nRF52840 AT") {
        return;
    }
    name->clear();
    if(default_node_name_from_netdev("eth0", name) ||
       default_node_name_from_netdev("wlan0", name)) {
        return;
    }
    (void)gethostname(host, sizeof(host));
    host[sizeof(host) - 1U] = '\0';
    snprintf(buf, sizeof(buf), "%s-%ld", host, (long)getpid());
    *name = buf;
}

static void default_from_node(probe_options_t *opts)
{
    uint32_t value;

    if(!opts || opts->from_node != 0U) {
        return;
    }
    value = djb2_hash(opts->node_name.c_str());
    value ^= 0x4b230000U;
    if(value == 0U || value == MESHTASTIC_NODENUM_BROADCAST) {
        value = 0x4b230001U;
    }
    opts->from_node = value;
}

static int16_t begin_chip(chip_type_t chip, PhysicalLayer *radio,
                          SX1262 *sx1262, LR2021 *lr2021,
                          const probe_profile_t *profile)
{
    int16_t state = RADIOLIB_ERR_CHIP_NOT_FOUND;

    if(!profile || !radio) {
        return state;
    }

    switch(chip) {
    case CHIP_SX1262:
        state = sx1262->begin(profile->freq, profile->bandwidth,
                              profile->sf, profile->cr, profile->sync_word,
                              profile->power, profile->preamble, 3.3f, false);
        if(state == RADIOLIB_ERR_NONE) {
            state = sx1262->setCRC(0);
        }
        break;
    case CHIP_LR2021:
        lr2021->irqDioNum = LORA_LR2021_IRQ_DIO_NUM;
        state = lr2021->begin(profile->freq, profile->bandwidth,
                              profile->sf, profile->cr, profile->sync_word,
                              profile->power, profile->preamble, 3.0f);
        if(state == RADIOLIB_ERR_NONE) {
            lr2021->setRfSwitchTable(lr2021_16e8_rf_switch_dio_pins,
                                     lr2021_16e8_rf_switch_table);
            printf("LR2021 16E8 RF switch: sub1G TX/RX DIO6=0 DIO7=0, 2.4G TX=01 RX=10\n");
            state = lr2021->setOutputPower(profile->power);
        }
        if(state == RADIOLIB_ERR_NONE) {
            state = lr2021->setCRC(0);
        }
        break;
    case CHIP_NONE:
    default:
        break;
    }
    return state;
}

static void lora_hard_reset(K230LinuxHal *hal, const char *reason)
{
    if(!hal) {
        return;
    }
    printf("LoRa hard reset: %s\n", reason ? reason : "-");
    hal->digitalWrite(LORA_PIN_POWER, K230_HAL_GPIO_LOW);
    hal->digitalWrite(LORA_PIN_RST, K230_HAL_GPIO_LOW);
    hal->delay(20);
    hal->digitalWrite(LORA_PIN_POWER, K230_HAL_GPIO_HIGH);
    hal->delay(20);
    hal->digitalWrite(LORA_PIN_RST, K230_HAL_GPIO_HIGH);
    hal->delay(80);
}

static void destroy_radio(PhysicalLayer **radio, SX1262 **sx1262,
                          LR2021 **lr2021, Module **module)
{
    if(*radio) {
        (*radio)->clearPacketReceivedAction();
        (*radio)->clearPacketSentAction();
        (void)(*radio)->standby();
    }
    delete *sx1262;
    delete *lr2021;
    delete *module;
    *radio = nullptr;
    *sx1262 = nullptr;
    *lr2021 = nullptr;
    *module = nullptr;
}

static int probe_candidate(chip_type_t chip, K230LinuxHal *hal,
                           const probe_profile_t *profile,
                           PhysicalLayer **radio_out, Module **module_out,
                           SX1262 **sx_out, LR2021 **lr_out,
                           int16_t *state_out)
{
    const unsigned int attempts = chip == CHIP_LR2021 ? 2U : 1U;
    int16_t last_state = RADIOLIB_ERR_CHIP_NOT_FOUND;

    for(unsigned int attempt = 0; attempt < attempts; attempt++) {
        Module *module = nullptr;
        SX1262 *sx1262 = nullptr;
        LR2021 *lr2021 = nullptr;
        PhysicalLayer *radio = nullptr;
        int16_t state;

        if(attempt > 0U) {
            lora_hard_reset(hal, "retry LR2021 probe");
        }

        module = new Module(hal, RADIOLIB_NC, LORA_PIN_DIO1,
                            LORA_PIN_RST, LORA_PIN_BUSY);
        if(!module) {
            last_state = RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
            break;
        }
        if(chip == CHIP_SX1262) {
            sx1262 = new SX1262(module);
            radio = sx1262;
        } else if(chip == CHIP_LR2021) {
            lr2021 = new LR2021(module);
            lr2021->irqDioNum = LORA_LR2021_IRQ_DIO_NUM;
            radio = lr2021;
        }
        if(!radio) {
            delete module;
            last_state = RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
            break;
        }

        hal->delay(20);
        state = begin_chip(chip, radio, sx1262, lr2021, profile);
        last_state = state;
        if(state == RADIOLIB_ERR_NONE) {
            *radio_out = radio;
            *module_out = module;
            *sx_out = sx1262;
            *lr_out = lr2021;
            return 0;
        }

        fprintf(stderr, "%s probe attempt %u/%u failed: %d %s\n",
                chip_name(chip), attempt + 1U, attempts, state,
                error_name(state));
        destroy_radio(&radio, &sx1262, &lr2021, &module);
        hal->delay(30);
    }

    if(state_out) {
        *state_out = last_state;
    }
    return -1;
}

static chip_type_t detect_radio(K230LinuxHal *hal, const probe_profile_t *profile,
                                PhysicalLayer **radio, Module **module,
                                SX1262 **sx1262, LR2021 **lr2021)
{
    int16_t sx_state = RADIOLIB_ERR_CHIP_NOT_FOUND;
    int16_t lr_state = RADIOLIB_ERR_CHIP_NOT_FOUND;

    if(probe_candidate(CHIP_SX1262, hal, profile, radio, module, sx1262,
                       lr2021, &sx_state) == 0) {
        printf("Detected SX1262\n");
        return CHIP_SX1262;
    }
    if(probe_candidate(CHIP_LR2021, hal, profile, radio, module, sx1262,
                       lr2021, &lr_state) == 0) {
        printf("Detected LR2021\n");
        return CHIP_LR2021;
    }

    fprintf(stderr, "LoRa probe failed: SX1262=%d %s LR2021=%d %s\n",
            sx_state, error_name(sx_state), lr_state, error_name(lr_state));
    return CHIP_NONE;
}

static int start_rx(PhysicalLayer *radio)
{
    int16_t state;

    if(!radio) {
        return -1;
    }
    active_op = OP_IDLE;
    take_radio_events();
    radio->clearPacketSentAction();
    radio->setPacketReceivedAction(radio_event_isr);
    if(active_lr2021) {
        state = active_lr2021->clearRxFifo();
        if(state != RADIOLIB_ERR_NONE) {
            fprintf(stderr, "LR2021 clear RX FIFO failed: %d %s\n",
                    state, error_name(state));
        }
    }
    state = radio->startReceive();
    if(state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "RX start failed: %d %s\n", state, error_name(state));
        return -1;
    }
    active_op = OP_RX;
    active_op_start_us = monotonic_us();
    return 0;
}

static int start_tx(PhysicalLayer *radio, const tx_frame_t &frame)
{
    int16_t state;
    size_t len = frame.bytes.size();

    if(!radio) {
        return -1;
    }
    if(len == 0U || len > MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        fprintf(stderr, "TX frame length invalid: %u\n", (unsigned)len);
        return -1;
    }
    take_radio_events();
    active_op = OP_IDLE;
    state = radio->standby();
    if(state == RADIOLIB_ERR_NONE) {
        if(active_lr2021) {
            int16_t fifo_state = active_lr2021->clearTxFifo();
            if(fifo_state != RADIOLIB_ERR_NONE) {
                fprintf(stderr, "LR2021 clear TX FIFO failed: %d %s\n",
                        fifo_state, error_name(fifo_state));
            }
        }
        radio->clearPacketReceivedAction();
        radio->setPacketSentAction(radio_event_isr);
        state = radio->startTransmit(frame.bytes.data(), len);
    }
    if(state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "TX start failed: %d %s\n", state, error_name(state));
        (void)start_rx(radio);
        return -1;
    }

    active_op = OP_TX;
    active_tx_len = len;
    active_op_start_us = monotonic_us();
    mesh_history_remember_tx(frame);
    daemon_event("TX start len=%u: %s", (unsigned)len,
                 frame.summary.c_str());
    return 0;
}

static void handle_rx_event(PhysicalLayer *radio, const probe_options_t &opts)
{
    uint8_t data[MESHTASTIC_MAX_LORA_PAYLOAD_LEN + 1U];
    tx_frame_t followup_frame;
    size_t len;
    int16_t state;
    bool rebroadcast_pending = false;

    if(!radio || active_op != OP_RX) {
        return;
    }

    memset(data, 0, sizeof(data));
    len = radio->getPacketLength();
    if(len >= sizeof(data)) {
        len = sizeof(data) - 1U;
    }
    state = radio->readData(data, len);
    active_op = OP_IDLE;
    if(state == RADIOLIB_ERR_NONE) {
        float rssi = radio->getRSSI();
        float snr = radio->getSNR();
        (void)radio->finishReceive();
        rx_count++;
        if(opts.mesh_mode) {
            rebroadcast_pending = process_mesh_rx(opts, data, len, rssi, snr,
                                                  &followup_frame);
        } else {
            for(size_t i = 0; i < len; i++) {
                if(data[i] < 32U || data[i] > 126U) {
                    data[i] = '.';
                }
            }
            data[len] = '\0';
            daemon_event("RX %lu len=%u rssi=%.1f snr=%.1f data=%s",
                         (unsigned long)rx_count, (unsigned)len, rssi, snr,
                         data);
        }
    } else {
        (void)radio->finishReceive();
        fprintf(stderr, "RX read failed: %d %s\n", state, error_name(state));
    }
    if(rebroadcast_pending) {
        if(followup_frame.routing_ack) {
            (void)mesh_delayed_ack_enqueue(followup_frame);
        } else {
            (void)mesh_delayed_tx_enqueue(followup_frame);
        }
    }
    (void)start_rx(radio);
}

static void handle_tx_event(PhysicalLayer *radio)
{
    int16_t state;

    if(!radio || active_op != OP_TX) {
        return;
    }

    state = radio->finishTransmit();
    active_op = OP_IDLE;
    if(active_lr2021) {
        int16_t fifo_state = active_lr2021->clearTxFifo();
        if(fifo_state != RADIOLIB_ERR_NONE) {
            fprintf(stderr, "LR2021 clear TX FIFO after finish failed: %d %s\n",
                    fifo_state, error_name(fifo_state));
        }
    }
    if(state == RADIOLIB_ERR_NONE) {
        tx_count++;
        daemon_event("TX done: %lu", (unsigned long)tx_count);
    } else {
        fprintf(stderr, "TX finish failed: %d %s\n", state, error_name(state));
    }
    (void)start_rx(radio);
}

static void handle_radio_event(PhysicalLayer *radio, const probe_options_t &opts)
{
    if(active_op == OP_TX) {
        handle_tx_event(radio);
    } else if(active_op == OP_RX) {
        handle_rx_event(radio, opts);
    }
}

static void handle_delayed_tx(PhysicalLayer *radio, uint64_t now_us)
{
    tx_frame_t frame;

    if(!radio || active_op == OP_TX) {
        return;
    }
    if(!mesh_delayed_tx_pop_due(now_us, &frame)) {
        return;
    }
    daemon_event("Mesh %s due id=0x%08x queued=%u",
                 frame.routing_ack ? "ACK" : "rebroadcast",
                 frame.packet_id, mesh_delayed_tx_count());
    if(start_tx(radio, frame) != 0) {
        if(frame.routing_ack) {
            mesh_ack_drop_count++;
            daemon_event("Mesh ACK TX start failed id=0x%08x drop=%lu",
                         frame.packet_id, (unsigned long)mesh_ack_drop_count);
        } else {
            mesh_rebroadcast_drop_count++;
            daemon_event("Mesh rebroadcast TX start failed id=0x%08x drop=%lu",
                         frame.packet_id,
                         (unsigned long)mesh_rebroadcast_drop_count);
        }
    } else if(frame.routing_ack && frame.want_ack) {
        (void)mesh_ack_track_frame(frame);
    }
}

static void handle_ack_retry(PhysicalLayer *radio, uint64_t now_us)
{
    size_t best = MESHTASTIC_ACK_RETRY_QUEUE_SIZE;

    if(!radio || active_op == OP_TX) {
        return;
    }
    for(size_t i = 0; i < MESHTASTIC_ACK_RETRY_QUEUE_SIZE; i++) {
        if(!mesh_ack_retry_queue[i].active ||
           mesh_ack_retry_queue[i].due_us > now_us) {
            continue;
        }
        if(best == MESHTASTIC_ACK_RETRY_QUEUE_SIZE ||
           mesh_ack_retry_queue[i].due_us < mesh_ack_retry_queue[best].due_us) {
            best = i;
        }
    }
    if(best == MESHTASTIC_ACK_RETRY_QUEUE_SIZE) {
        return;
    }
    if(mesh_ack_retry_queue[best].retries_left == 0U) {
        mesh_ack_timeout_count++;
        daemon_event("Mesh ACK timeout id=0x%08x to=0x%08x timeout=%lu pending=%u",
                     mesh_ack_retry_queue[best].packet_id,
                     mesh_ack_retry_queue[best].to_node,
                     (unsigned long)mesh_ack_timeout_count,
                     mesh_ack_pending_count() - 1U);
        mesh_ack_retry_queue[best].active = false;
        mesh_ack_retry_queue[best].due_us = 0;
        return;
    }

    mesh_ack_retry_queue[best].retries_left--;
    mesh_ack_retry_queue[best].due_us =
        now_us + MESHTASTIC_ACK_RETRY_TIMEOUT_US;
    mesh_ack_retry_count++;
    daemon_event("Mesh ACK retry id=0x%08x to=0x%08x retries_left=%u retry=%lu",
                 mesh_ack_retry_queue[best].packet_id,
                 mesh_ack_retry_queue[best].to_node,
                 mesh_ack_retry_queue[best].retries_left,
                 (unsigned long)mesh_ack_retry_count);
    if(start_tx(radio, mesh_ack_retry_queue[best].frame) != 0) {
        mesh_ack_retry_queue[best].due_us =
            now_us + (MESHTASTIC_ACK_RETRY_TIMEOUT_US / 2ULL);
        daemon_event("Mesh ACK retry TX start failed id=0x%08x to=0x%08x",
                     mesh_ack_retry_queue[best].packet_id,
                     mesh_ack_retry_queue[best].to_node);
    }
}

int main(int argc, char **argv)
{
    probe_options_t opts;
    K230LinuxHal *hal = nullptr;
    Module *module = nullptr;
    SX1262 *sx1262 = nullptr;
    LR2021 *lr2021 = nullptr;
    PhysicalLayer *radio = nullptr;
    chip_type_t chip = CHIP_NONE;
    uint64_t start_us;
    uint64_t last_tx_us = 0;
    bool send_once_started = false;
    bool send_once_finished = false;
    bool send_once_awaiting_ack = false;
    int daemon_fd = -1;
    std::string pending_daemon_send;

    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    if(!parse_options(argc, argv, &opts)) {
        return 2;
    }
    {
        int client_commands = (opts.client_status ? 1 : 0) +
                              (opts.client_log ? 1 : 0) +
                              (opts.client_chat ? 1 : 0) +
                              (opts.client_nodes ? 1 : 0) +
                              (opts.client_quit ? 1 : 0) +
                              (opts.client_send_requested ? 1 : 0);
        if(client_commands > 1) {
            fprintf(stderr, "Only one --cmd-* option can be used at once\n");
            return 2;
        }
        if(client_commands == 1) {
            return run_daemon_client(opts);
        }
    }
    default_node_name(&opts.node_name);
    default_from_node(&opts);
    if(opts.mesh_mode && !opts.want_ack_set &&
       opts.to_node != MESHTASTIC_NODENUM_BROADCAST) {
        opts.want_ack = true;
    }
    if(!apply_meshtastic_profile(&opts)) {
        return 2;
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("k230_meshtastic_probe %s\n", PROBE_VERSION);
    printf("Pins: spi=%s hw_cs=GPIO%u rst=GPIO%u busy=GPIO%u irq_gpio=GPIO%u lr2021_irq_dio=%u power=GPIO%u\n",
           opts.spi_path.c_str(), LORA_PIN_CS, LORA_PIN_RST, LORA_PIN_BUSY,
           LORA_PIN_DIO1, LORA_LR2021_IRQ_DIO_NUM, LORA_PIN_POWER);
    printf("Profile: freq=%.3fMHz bw=%.1fkHz sf=%u cr=4/%u sw=0x%02X power=%d preamble=%u node=%s\n",
           opts.profile.freq, opts.profile.bandwidth, opts.profile.sf,
           opts.profile.cr, opts.profile.sync_word, opts.profile.power,
           opts.profile.preamble, opts.node_name.c_str());
    if(!opts.resolved_region.empty()) {
        printf("Meshtastic profile: region=%s preset=%s slot=%u/%u channel_name=%s effective_name=%s\n",
               opts.resolved_region.c_str(), opts.resolved_preset.c_str(),
               opts.resolved_slot, opts.resolved_slot_count,
               opts.channel_name.empty() ? "<default>" : opts.channel_name.c_str(),
               effective_mesh_channel_name(opts).c_str());
    }
    if(opts.mesh_mode) {
        std::vector<uint8_t> key;
        std::string channel_name = effective_mesh_channel_name(opts);
        if(!parse_psk(opts.psk, &key)) {
            fprintf(stderr, "Invalid --psk value. Use default, none, or 16/32-byte hex.\n");
            return 2;
        }
        printf("Mesh frame: from=0x%08x to=0x%08x hop=%u want_ack=%s rebroadcast=%s channel_name='%s' psk=%s channel_hash=0x%02x\n",
               opts.from_node, opts.to_node, opts.hop_limit,
               opts.want_ack ? "on" : "off",
               opts.rebroadcast ? "on" : "off",
               channel_name.c_str(), key.empty() ? "none" : opts.psk.c_str(),
               mesh_channel_hash(channel_name, key));
    }

    hal = new K230LinuxHal(opts.spi_path.c_str(), LORA_SPI_SPEED_HZ);
    if(!hal) {
        fprintf(stderr, "HAL allocation failed\n");
        return 1;
    }
    hal->pinMode(LORA_PIN_POWER, K230_HAL_GPIO_OUTPUT);
    hal->digitalWrite(LORA_PIN_POWER, K230_HAL_GPIO_HIGH);
    hal->delay(30);
    hal->spiBegin();
    if(!hal->spi_ready()) {
        fprintf(stderr, "SPI not ready: %s\n", hal->last_error());
        delete hal;
        return 1;
    }

    chip = detect_radio(hal, &opts.profile, &radio, &module, &sx1262, &lr2021);
    if(chip == CHIP_NONE || !radio) {
        delete hal;
        return 1;
    }
    active_lr2021 = (chip == CHIP_LR2021) ? lr2021 : nullptr;
    printf("Radio ready: %s\n", chip_name(chip));

    if(start_rx(radio) != 0) {
        destroy_radio(&radio, &sx1262, &lr2021, &module);
        delete hal;
        return 1;
    }
    if(opts.daemon_mode) {
        daemon_fd = setup_daemon_socket(opts.socket_path);
        if(daemon_fd < 0) {
            destroy_radio(&radio, &sx1262, &lr2021, &module);
            delete hal;
            return 1;
        }
        printf("Daemon socket: %s\n", opts.socket_path.c_str());
        daemon_event("Daemon ready chip=%s region=%s preset=%s freq=%.3f",
                     chip_name(chip),
                     opts.resolved_region.empty() ? "-" :
                     opts.resolved_region.c_str(),
                     opts.resolved_preset.empty() ? "-" :
                     opts.resolved_preset.c_str(),
                     opts.profile.freq);
        mesh_next_nodeinfo_us =
            (opts.mesh_mode && opts.advertise_nodeinfo) ? monotonic_us() : 0ULL;
        if(opts.mesh_mode) {
            phoneapi_start(opts);
        }
    }
    printf("Listening. Press Ctrl-C to stop.\n");

    start_us = monotonic_us();
    while(running) {
        uint64_t now = monotonic_us();
        unsigned int events = take_radio_events();

        if(events == 0U && active_op == OP_RX &&
           hal && hal->digitalRead(LORA_PIN_DIO1) == K230_HAL_GPIO_HIGH) {
            events = 1U;
            printf("RX DIO poll event\n");
        }

        if(events > 0U) {
            if(active_op == OP_TX &&
               !elapsed_after(now, active_op_start_us,
                              tx_min_finish_delay_us(active_tx_len))) {
                printf("TX event ignored before air-time guard: elapsed=%llu guard=%llu events=%u\n",
                       (unsigned long long)(now >= active_op_start_us ?
                                            now - active_op_start_us : 0),
                       (unsigned long long)tx_min_finish_delay_us(active_tx_len),
                       events);
            } else {
                handle_radio_event(radio, opts);
            }
            if(active_op != OP_TX && !opts.auto_tx && send_once_started &&
               !send_once_finished &&
               (!send_once_awaiting_ack || mesh_ack_pending_count() == 0U)) {
                send_once_finished = true;
            }
        }

        if(daemon_fd >= 0) {
            accept_daemon_clients(daemon_fd, opts, chip, &pending_daemon_send);
        }

        if(active_op != OP_TX) {
            probe_options_t updated_opts;
            bool request_reconfigure = false;

            if(phoneapi_take_runtime_opts(&updated_opts,
                                          &request_reconfigure)) {
                opts = updated_opts;
                if(request_reconfigure) {
                    int16_t state;

                    active_op = OP_IDLE;
                    (void)radio->standby();
                    state = begin_chip(chip, radio, sx1262, lr2021,
                                       &opts.profile);
                    if(state == RADIOLIB_ERR_NONE && start_rx(radio) == 0) {
                        daemon_event("PhoneAPI radio reconfigured region=%s preset=%s freq=%.3f power=%d",
                                     opts.resolved_region.empty() ? "-" :
                                     opts.resolved_region.c_str(),
                                     opts.resolved_preset.empty() ? "-" :
                                     opts.resolved_preset.c_str(),
                                     opts.profile.freq, opts.profile.power);
                    } else {
                        daemon_event("PhoneAPI radio reconfigure failed state=%d",
                                     state);
                    }
                }
            }
        }

        mesh_history_expire(now);
        handle_delayed_tx(radio, now);
        handle_ack_retry(radio, now);

        if(mesh_next_nodeinfo_us != 0ULL && now >= mesh_next_nodeinfo_us &&
           active_op != OP_TX) {
            tx_frame_t frame;
            uint64_t interval_us =
                (uint64_t)opts.nodeinfo_interval_sec * 1000000ULL;
            if(build_mesh_nodeinfo_frame(opts, &frame)) {
                if(start_tx(radio, frame) == 0) {
                    mesh_nodeinfo_tx_count++;
                    mesh_next_nodeinfo_us = now + interval_us;
                    daemon_event("NodeInfo TX start node=%s from=0x%08x",
                                 opts.node_name.c_str(), opts.from_node);
                } else {
                    mesh_nodeinfo_drop_count++;
                    mesh_next_nodeinfo_us = now + MESHTASTIC_NODEINFO_RETRY_US;
                    daemon_event("NodeInfo TX start failed node=%s",
                                 opts.node_name.c_str());
                }
            } else {
                mesh_nodeinfo_drop_count++;
                mesh_next_nodeinfo_us = now + MESHTASTIC_NODEINFO_RETRY_US;
                daemon_event("NodeInfo build failed node=%s",
                             opts.node_name.c_str());
            }
        }

        if(!pending_daemon_send.empty() && active_op != OP_TX) {
            tx_frame_t frame;
            std::string message = pending_daemon_send;
            pending_daemon_send.clear();
            if(build_tx_frame(opts, message, &frame)) {
                if(start_tx(radio, frame) == 0 && opts.mesh_mode) {
                    if(frame.want_ack) {
                        (void)mesh_ack_track_frame(frame);
                    }
                    std::string clean = mesh_clean_text(message);
                    if(!clean.empty()) {
                        daemon_chat("TX 0x%08x: %s", opts.from_node,
                                    clean.c_str());
                    }
                }
            }
        }

        if(active_op != OP_TX) {
            phoneapi_mesh_tx_t phoneapi_tx;
            if(phoneapi_take_mesh_tx(&phoneapi_tx)) {
                tx_frame_t frame;
                if(build_phoneapi_mesh_data_frame(opts, phoneapi_tx, &frame)) {
                    if(start_tx(radio, frame) == 0 && opts.mesh_mode) {
                        if(frame.want_ack) {
                            (void)mesh_ack_track_frame(frame);
                        }
                        if(phoneapi_tx.data.portnum ==
                           MESHTASTIC_TEXT_MESSAGE_APP &&
                           !phoneapi_tx.data.payload.empty()) {
                            std::string text(
                                (const char *)phoneapi_tx.data.payload.data(),
                                phoneapi_tx.data.payload.size());
                            std::string clean = mesh_clean_text(text);
                            if(!clean.empty()) {
                                daemon_chat("TX 0x%08x: %s", opts.from_node,
                                            clean.c_str());
                            }
                        }
                    }
                } else {
                    daemon_event("PhoneAPI TX build failed port=%u payload=%u",
                                 phoneapi_tx.data.portnum,
                                 (unsigned)phoneapi_tx.data.payload.size());
                }
            }
        }

        if(!opts.send_once.empty() && !send_once_started &&
           active_op != OP_TX) {
            tx_frame_t frame;
            if(build_tx_frame(opts, opts.send_once, &frame) &&
               start_tx(radio, frame) == 0) {
                send_once_started = true;
                send_once_awaiting_ack = frame.want_ack;
                if(frame.want_ack) {
                    (void)mesh_ack_track_frame(frame);
                }
            } else {
                send_once_finished = true;
            }
        }

        if(opts.auto_tx && active_op != OP_TX &&
           now - last_tx_us >= (uint64_t)opts.interval_ms * 1000ULL) {
            tx_frame_t frame;
            last_tx_us = now;
            if(build_tx_frame(opts, opts.message, &frame)) {
                if(start_tx(radio, frame) == 0 && frame.want_ack) {
                    (void)mesh_ack_track_frame(frame);
                }
            }
        }

        if(active_op == OP_TX &&
           elapsed_after(now, active_op_start_us,
                         tx_poll_finish_delay_us(active_tx_len))) {
            handle_tx_event(radio);
            if(send_once_started &&
               (!send_once_awaiting_ack || mesh_ack_pending_count() == 0U)) {
                send_once_finished = true;
            }
        }

        if(active_op == OP_TX &&
           elapsed_after(now, active_op_start_us, 15000000ULL)) {
            fprintf(stderr, "TX timeout watchdog\n");
            active_op = OP_IDLE;
            (void)radio->standby();
            (void)start_rx(radio);
            if(send_once_started &&
               (!send_once_awaiting_ack || mesh_ack_pending_count() == 0U)) {
                send_once_finished = true;
            }
        }

        if(!opts.auto_tx && !opts.send_once.empty() && send_once_finished &&
           opts.duration_sec == 0U) {
            break;
        }

        if(opts.duration_sec > 0U &&
           now - start_us >= (uint64_t)opts.duration_sec * 1000000ULL) {
            break;
        }

        usleep(10000);
    }

    printf("Summary: chip=%s tx=%lu rx=%lu\n", chip_name(chip),
           (unsigned long)tx_count, (unsigned long)rx_count);
    phoneapi_stop();
    if(daemon_fd >= 0) {
        close(daemon_fd);
        unlink(opts.socket_path.c_str());
        daemon_fd = -1;
    }
    destroy_radio(&radio, &sx1262, &lr2021, &module);
    delete hal;
    return 0;
}
