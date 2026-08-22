#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <math.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <codec2.h>
#include <jpeglib.h>
#include <opus/opus.h>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "modules/LR2021/LR2021.h"
#include "modules/SX126x/SX1262.h"
#include "ui_nrf9151_manager.h"

#define PROBE_VERSION "0.31"
#define LORA_SPI_DEV "/dev/spidev0.0"
#define LORA_SPI_SPEED_HZ 4000000U
#define MESHTASTIC_DAEMON_SEND_QUEUE_MAX 16U
#define MESHTASTIC_DAEMON_REQUEST_QUEUE_MAX 8U
#define MESHTASTIC_REMOTE_STATUS_HISTORY_MAX 16U
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
#define MESHTASTIC_TEXT_MESSAGE_COMPRESSED_APP 7U
#define MESHTASTIC_WAYPOINT_APP 8U
#define MESHTASTIC_AUDIO_APP 9U
#define MESHTASTIC_TELEMETRY_APP 67U
#define MESHTASTIC_TRACEROUTE_APP 70U
#define MESHTASTIC_NEIGHBORINFO_APP 71U
#define MESHTASTIC_PRIVATE_APP 256U
#define MESHTASTIC_ERRNO_SHOULD_RELEASE 35U
#define MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK 0x07U
#define MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK 0x08U
#define MESHTASTIC_PACKET_FLAGS_HOP_START_MASK 0xE0U
#define MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT 5U
#define MESHTASTIC_ROUTING_ERROR_NONE 0U
#define MESHTASTIC_ROUTING_ERROR_NO_ROUTE 1U
#define MESHTASTIC_ROUTING_ERROR_GOT_NAK 2U
#define MESHTASTIC_ROUTING_ERROR_TIMEOUT 3U
#define MESHTASTIC_ROUTING_ERROR_NO_INTERFACE 4U
#define MESHTASTIC_ROUTING_ERROR_MAX_RETRANSMIT 5U
#define MESHTASTIC_ROUTING_ERROR_NO_CHANNEL 6U
#define MESHTASTIC_ROUTING_ERROR_TOO_LARGE 7U
#define MESHTASTIC_ROUTING_ERROR_NO_RESPONSE 8U
#define MESHTASTIC_ROUTING_ERROR_DUTY_CYCLE_LIMIT 9U
#define MESHTASTIC_SYNC_WORD 0x2BU
#define MESHTASTIC_DEFAULT_REGION "US"
#define MESHTASTIC_DEFAULT_PRESET "LONG_FAST"
#define MESHTASTIC_DEFAULT_SOCKET_PATH "/tmp/k230_meshtastic.sock"
#define MESHTASTIC_MAX_IPC_MESSAGE_LEN 220U
#define MESHTASTIC_EVENT_LOG_LINES 32U
#define MESHTASTIC_EVENT_LOG_LINE_LEN 192U
#define MESHTASTIC_CHAT_LOG_LINES 24U
#define MESHTASTIC_CHAT_LOG_LINE_LEN 256U
#define MESHTASTIC_NODE_CACHE_SIZE 24U
#define MESHTASTIC_WAYPOINT_CACHE_SIZE 16U
#define MESHTASTIC_PACKET_HISTORY_SIZE 64U
#define MESHTASTIC_PACKET_HISTORY_TTL_US (30ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_NODE_ACTIVE_5M_S 300U
#define MESHTASTIC_NODE_ACTIVE_15M_S 900U
#define MESHTASTIC_CHAT_DEDUP_SIZE 16U
#define MESHTASTIC_CHAT_DEDUP_TTL_US (45ULL * 1000000ULL)
#define MESHTASTIC_VOICE_MAGIC "KPV1"
#define MESHTASTIC_VOICE_CODEC_OPUS 1U
#define MESHTASTIC_VOICE_CODEC2_HEADER_LEN 4U
#define MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE CODEC2_MODE_1200
#define MESHTASTIC_VOICE_SAMPLE_RATE 8000U
#define MESHTASTIC_VOICE_FRAME_MS 20U
#define MESHTASTIC_VOICE_FRAME_SAMPLES \
    ((MESHTASTIC_VOICE_SAMPLE_RATE * MESHTASTIC_VOICE_FRAME_MS) / 1000U)
#define MESHTASTIC_VOICE_BITRATE_BPS 3600
#define MESHTASTIC_VOICE_MAX_SECONDS 10U
#define MESHTASTIC_VOICE_MAX_PCM_BYTES \
    (MESHTASTIC_VOICE_SAMPLE_RATE * 2U * MESHTASTIC_VOICE_MAX_SECONDS)
#define MESHTASTIC_VOICE_CHUNK_TARGET_BYTES 220U
#define MESHTASTIC_VOICE_RX_STREAMS 4U
#define MESHTASTIC_FLRC_VOICE_MAGIC 0x3156464BU
#define MESHTASTIC_FLRC_VOICE_TYPE_INVITE 1U
#define MESHTASTIC_FLRC_VOICE_TYPE_DATA 2U
#define MESHTASTIC_FLRC_VOICE_TYPE_DONE 3U
#define MESHTASTIC_FLRC_VOICE_HDR_LEN 32U
#define MESHTASTIC_FLRC_VOICE_PACKET_LEN 252U
#define MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN \
    (MESHTASTIC_FLRC_VOICE_PACKET_LEN - MESHTASTIC_FLRC_VOICE_HDR_LEN)
#define MESHTASTIC_FLRC_VOICE_FREQ_MHZ 2400.0f
#define MESHTASTIC_FLRC_VOICE_FREQ_TENTHS 24000U
#define MESHTASTIC_FLRC_VOICE_BR_KBPS 2600U
#define MESHTASTIC_FLRC_VOICE_POWER_DBM 8
#define MESHTASTIC_FLRC_VOICE_PREAMBLE 16U
#define MESHTASTIC_FLRC_VOICE_SYNC_LEN 4U
#define MESHTASTIC_FLRC_VOICE_RX_GUARD_MS 2600U
#define MESHTASTIC_FLRC_VOICE_TX_START_DELAY_US 220000ULL
#define MESHTASTIC_FLRC_VOICE_PACKET_GAP_US 2500U
#define MESHTASTIC_FLRC_VOICE_START_REPEAT 3U
#define MESHTASTIC_FLRC_VOICE_DATA_REPEAT 2U
#define MESHTASTIC_FLRC_VOICE_DONE_REPEAT 3U
#define MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG 0x81U
#define MESHTASTIC_FLRC_PHOTO_MAX_W 680U
#define MESHTASTIC_FLRC_PHOTO_MAX_H 480U
#define MESHTASTIC_FLRC_PHOTO_JPEG_QUALITY 42
#define MESHTASTIC_FLRC_PHOTO_MAX_PACKETS 512U
#define MESHTASTIC_FLRC_PHOTO_MAX_BYTES \
    (MESHTASTIC_FLRC_PHOTO_MAX_PACKETS * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN)
#define MESHTASTIC_FLRC_PHOTO_PACKET_GAP_US 5000U
#define MESHTASTIC_FLRC_PHOTO_ROUND_GAP_US 25000U
#define MESHTASTIC_FLRC_PHOTO_DATA_REPEAT 2U
#define MESHTASTIC_FLRC_PHOTO_START_REPEAT 4U
#define MESHTASTIC_FLRC_PHOTO_DONE_REPEAT 5U
#define MESHTASTIC_FLRC_PHOTO_REPAIR_MAGIC 0x3150464BU
#define MESHTASTIC_FLRC_PHOTO_REPAIR_TYPE_REQ 1U
#define MESHTASTIC_FLRC_PHOTO_REPAIR_HDR_LEN 28U
#define MESHTASTIC_FLRC_PHOTO_REPAIR_MAX_ROUNDS 2U
#define MESHTASTIC_FLRC_PHOTO_REPAIR_DATA_REPEAT 2U
#define MESHTASTIC_FLRC_PHOTO_REPAIR_START_DELAY_US 220000ULL
#define MESHTASTIC_FLRC_PHOTO_REPAIR_WINDOW_US 7000000ULL
#define MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_SIZE 4U
#define MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_TTL_US \
    (5ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_FLRC_PHOTO_REPAIR_WINDOW_MS \
    (MESHTASTIC_FLRC_PHOTO_REPAIR_WINDOW_US / 1000ULL)
#define MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_TTL_SEC \
    (MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_TTL_US / 1000000ULL)
#define MESHTASTIC_FLRC_PHOTO_DEBUG_DROP_MAX_SEQ 64U
#define MESHTASTIC_PHOTO_SOURCE_DIR "/root/photos"
#define MESHTASTIC_PHOTO_STORE_DIR "/root/meshtastic/photos"
#define MESHTASTIC_AIRTIME_CHANNEL_PERIODS 6U
#define MESHTASTIC_AIRTIME_CHANNEL_PERIOD_US (10ULL * 1000000ULL)
#define MESHTASTIC_AIRTIME_TX_PERIODS 60U
#define MESHTASTIC_AIRTIME_TX_PERIOD_US (60ULL * 1000000ULL)
#define MESHTASTIC_AIRTIME_POLITE_CHANNEL_UTIL_PERCENT 25.0f
#define MESHTASTIC_AIRTIME_MAX_CHANNEL_UTIL_PERCENT 40.0f
#define MESHTASTIC_AIRTIME_POLITE_DUTY_CYCLE_RATIO 0.5f
#define MESHTASTIC_DELAYED_TX_QUEUE_SIZE 4U
#define MESHTASTIC_REBROADCAST_MIN_DELAY_US 150000ULL
#define MESHTASTIC_REBROADCAST_JITTER_US 700000ULL
#define MESHTASTIC_ACK_RESPONSE_DELAY_US 5500000ULL
#define MESHTASTIC_ACK_RETRY_QUEUE_SIZE 4U
#define MESHTASTIC_ACK_RETRY_TIMEOUT_US 12000000ULL
#define MESHTASTIC_ACK_RETRY_MAX 3U
#define MESHTASTIC_NODEINFO_INTERVAL_US (10ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_NODEINFO_RETRY_US (60ULL * 1000000ULL)
#define MESHTASTIC_POSITION_INTERVAL_US (15ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_POSITION_RETRY_US (30ULL * 1000000ULL)
#define MESHTASTIC_DEVICE_TELEMETRY_INTERVAL_US (5ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_ENV_TELEMETRY_INTERVAL_US (5ULL * 60ULL * 1000000ULL)
#define MESHTASTIC_TELEMETRY_RETRY_US (60ULL * 1000000ULL)
#define MESHTASTIC_PHONEAPI_UART_DEV "/dev/ttyS1"
#define MESHTASTIC_PHONEAPI_FROM_SEND_GAP_US 120000U
#define MESHTASTIC_NRF9151_UART_DEV K230_NRF9151_UART_DEV
#define MESHTASTIC_NRF9151_UART_BAUD B115200
#define MESHTASTIC_NRF9151_FIX_CACHE_MAX_AGE_SEC (60 * 60)
#define MESHTASTIC_NRF9151_PROBE_TIMEOUT_US 500000ULL
#define MESHTASTIC_NRF9151_CMD_TIMEOUT_US 1800000ULL
#define MESHTASTIC_NRF9151_SEARCH_RESTART_US (120ULL * 1000000ULL)
#define MESHTASTIC_NRF9151_SEARCH_RESTART_GAP_US (180ULL * 1000000ULL)
#define MESHTASTIC_NRF9151_LINE_MAX 256U
#define MESHTASTIC_NRF9151_RESPONSE_MAX 1024U
#define MESHTASTIC_AHT20_I2C_DEV "/dev/i2c-0"
#define MESHTASTIC_AHT20_ADDR 0x38
#define MESHTASTIC_AHT20_STATUS_BUSY 0x80
#define MESHTASTIC_AHT20_STATUS_CALIBRATED 0x08
#define MESHTASTIC_I2C4_IOMUX_BASE 0x91105000UL
#define MESHTASTIC_I2C4_IOMUX_SIZE 0x1000UL
#define MESHTASTIC_I2C4_IOMUX_IO46_OFFSET (46U * 4U)
#define MESHTASTIC_I2C4_IOMUX_IO47_OFFSET (47U * 4U)
#define MESHTASTIC_I2C4_GPIO_CHIP "/dev/gpiochip1"
#define MESHTASTIC_I2C4_GPIO_SCL_OFFSET 14U
#define MESHTASTIC_I2C4_GPIO_SDA_OFFSET 15U
#define MESHTASTIC_BQ27220_ADDR 0x55
#define MESHTASTIC_BQ27220_REG_VOLTAGE 0x08
#define MESHTASTIC_BQ27220_REG_CURRENT 0x0C
#define MESHTASTIC_BQ27220_REG_SOC 0x2C
#define MESHTASTIC_BQ27220_CACHE_TTL_US (60ULL * 60ULL * 1000000ULL)
#define K230_PHONE_UI_CONFIG_PARENT "/root/.config"
#define K230_PHONE_UI_PREFS_DIR K230_PHONE_UI_CONFIG_PARENT "/k230_phone_ui"
#define K230_MESH_NODEDB_FILE K230_PHONE_UI_PREFS_DIR "/meshtastic_nodes.tsv"
#define K230_MESH_NODEDB_TMP_FILE K230_MESH_NODEDB_FILE ".tmp"
#define K230_MESH_IDENTITY_FILE K230_PHONE_UI_PREFS_DIR "/meshtastic_identity.tsv"
#define K230_MESH_UI_DIR "/root/meshtastic"
#define K230_MESH_CANNED_MESSAGES_FILE K230_MESH_UI_DIR "/canned_messages.txt"
#define K230_MESH_RINGTONE_FILE K230_MESH_UI_DIR "/ringtone.rtttl"
#define K230_MESH_CANNED_MESSAGES_MAX_BYTES 200U
#define K230_MESH_CANNED_MESSAGES_MAX_ITEMS 8U
#define K230_MESH_RINGTONE_MAX_BYTES 230U
#define MESHTASTIC_PHONEAPI_MAX_CHANNELS 8U
#define MESHTASTIC_CHANNEL_ROLE_DISABLED 0U
#define MESHTASTIC_CHANNEL_ROLE_PRIMARY 1U
#define MESHTASTIC_CHANNEL_ROLE_SECONDARY 2U
#define MESHTASTIC_NODEDB_SAVE_DEBOUNCE_US (5ULL * 1000000ULL)
#define K230_MESH_IOMUX_BASE 0x91105000UL
#define K230_MESH_IOMUX_SIZE 0x1000UL
#define K230_MESH_IOMUX_IO28_OFFSET (28U * 4U)
#define K230_MESH_IOMUX_IO29_OFFSET (29U * 4U)
#define K230_MESH_IOMUX_IO50_OFFSET (50U * 4U)
#define K230_MESH_IOMUX_IO51_OFFSET (51U * 4U)
#define K230_MESH_IOMUX_FUNC_ALT0 (0U << 11)
#define K230_MESH_IOMUX_FUNC_ALT2 (2U << 11)
#define K230_MESH_IOMUX_IE_BIT (1U << 8)
#define K230_MESH_IOMUX_OE_BIT (1U << 7)
#define K230_MESH_IOMUX_ST_BIT (1U << 0)
#define K230_MESH_IOMUX_DS_8MA (8U << 1)
#define MESHTASTIC_PHONEAPI_ADV_REFRESH_US (15ULL * 1000000ULL)
#define MESHTASTIC_PHONEAPI_CONFIG_NONCE 69420U
#define MESHTASTIC_PHONEAPI_NODEINFO_NONCE 69421U
#define MESHTASTIC_HW_MODEL_NRF52840_PCA10059 40U
#define MESHTASTIC_MAX_K230_TX_POWER_DBM 22
#define MESHTASTIC_PKC_OVERHEAD 12U
#define MESHTASTIC_CURVE25519_KEY_LEN 32U
#define MESHTASTIC_PKC_TAG_LEN 8U
#define MESHTASTIC_PKC_NONCE_LEN 13U
#define OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH 0
#define OVERRIDE_SLOT_PRESET_HASH -1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static bool meshtastic_node_is_broadcast(uint32_t node)
{
    return node == MESHTASTIC_NODENUM_BROADCAST;
}

static bool meshtastic_port_is_text(uint32_t portnum)
{
    return portnum == MESHTASTIC_TEXT_MESSAGE_APP ||
           portnum == MESHTASTIC_TEXT_MESSAGE_COMPRESSED_APP;
}

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
    bool configured = false;
    uint32_t role = MESHTASTIC_CHANNEL_ROLE_DISABLED;
    std::string name;
    std::string psk;
    bool uplink_enabled = false;
    bool downlink_enabled = false;
    bool has_position_precision = false;
    uint32_t position_precision = 0;
    bool is_muted = false;
} mesh_channel_slot_t;

typedef struct {
    probe_profile_t profile;
    std::string spi_path = LORA_SPI_DEV;
    std::string node_name;
    std::string send_once;
    std::string message = "hello from k230";
    std::string channel_name;
    std::string psk = "default";
    std::string socket_path = MESHTASTIC_DEFAULT_SOCKET_PATH;
    std::string gps_uart_path = MESHTASTIC_NRF9151_UART_DEV;
    std::string client_send;
    bool auto_tx = false;
    bool mesh_mode = false;
    bool daemon_mode = false;
    bool client_send_requested = false;
    bool client_status = false;
    bool client_log = false;
    bool client_chat = false;
    bool client_nodes = false;
    bool client_map = false;
    bool client_waypoints = false;
    bool client_channels = false;
    bool client_set_channel_slot = false;
    bool client_channel_url = false;
    bool client_quit = false;
    bool client_publish_nodeinfo = false;
    bool client_send_to_requested = false;
    bool client_send_to_ack = false;
    std::string client_send_to_target;
    std::string client_send_to_message;
    bool client_send_voice_requested = false;
    std::string client_send_voice_path;
    bool client_send_waypoint_requested = false;
    std::string client_send_waypoint_text;
    std::string client_set_channel_slot_command;
    bool client_publish_position = false;
    bool client_publish_telemetry = false;
    bool client_request_nodeinfo = false;
    bool client_request_position = false;
    bool client_request_telemetry = false;
    bool client_request_traceroute = false;
    bool client_request_neighborinfo = false;
    bool client_request_status = false;
    std::string client_request_target;
    bool client_import_node_key = false;
    std::string client_import_node;
    std::string client_import_key;
    bool rebroadcast = true;
    bool advertise_nodeinfo = true;
    bool phoneapi_enabled = true;
    bool position_enabled = true;
    bool telemetry_enabled = true;
    bool environment_telemetry_enabled = true;
    bool fixed_position_enabled = false;
    bool fixed_position_has_altitude = false;
    bool want_ack = false;
    bool want_ack_set = false;
    int32_t fixed_position_latitude_i = 0;
    int32_t fixed_position_longitude_i = 0;
    int32_t fixed_position_altitude_m = 0;
    uint32_t from_node = 0;
    uint32_t to_node = MESHTASTIC_NODENUM_BROADCAST;
    uint32_t packet_id = 0;
    uint8_t hop_limit = 3;
    uint32_t interval_ms = 1000;
    uint32_t duration_sec = 0;
    uint32_t nodeinfo_interval_sec =
        (uint32_t)(MESHTASTIC_NODEINFO_INTERVAL_US / 1000000ULL);
    uint32_t position_interval_sec =
        (uint32_t)(MESHTASTIC_POSITION_INTERVAL_US / 1000000ULL);
    uint32_t telemetry_device_interval_sec =
        (uint32_t)(MESHTASTIC_DEVICE_TELEMETRY_INTERVAL_US / 1000000ULL);
    uint32_t telemetry_environment_interval_sec =
        (uint32_t)(MESHTASTIC_ENV_TELEMETRY_INTERVAL_US / 1000000ULL);
    mesh_channel_slot_t channels[MESHTASTIC_PHONEAPI_MAX_CHANNELS];
    uint32_t primary_channel_index = 0;
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
    bool valid = false;
    uint32_t index = 0;
    uint32_t role = MESHTASTIC_CHANNEL_ROLE_DISABLED;
    uint8_t hash = 0;
    std::string name;
    std::string psk;
} mesh_channel_match_t;

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
    uint32_t last_seen_epoch = 0;
    int rssi_dbm = 0;
    float snr = 0.0f;
    uint32_t rx_count = 0;
    bool has_channel = false;
    uint32_t channel_index = 0;
    bool has_hops_away = false;
    uint32_t hops_away = 0;
    char long_name[40] = {0};
    char short_name[8] = {0};
    int hw_model = -1;
    bool has_public_key = false;
    uint8_t public_key[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
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
    bool has_route_info = false;
    char route_summary[160] = {0};
    bool is_favorite = false;
    bool is_ignored = false;
    bool is_muted = false;
} mesh_node_entry_t;

typedef struct {
    std::vector<uint8_t> bytes;
    std::string summary;
    std::vector<uint8_t> flrc_voice_payload;
    std::vector<uint8_t> flrc_photo_payload;
    bool rebroadcast = false;
    bool want_ack = false;
    bool routing_ack = false;
    bool phoneapi_origin = false;
    bool flrc_voice_after_tx = false;
    bool flrc_photo_after_tx = false;
    uint32_t rebroadcast_from = 0;
    uint32_t to_node = 0;
    uint32_t from_node = 0;
    uint32_t packet_id = 0;
    uint32_t ack_request_id = 0;
    uint32_t flrc_voice_stream_id = 0;
    uint32_t flrc_voice_duration_ms = 0;
    uint32_t flrc_voice_payload_crc = 0;
    uint16_t flrc_voice_total_packets = 0;
    uint8_t flrc_voice_codec_mode = 0;
    uint32_t flrc_photo_stream_id = 0;
    uint32_t flrc_photo_payload_crc = 0;
    uint16_t flrc_photo_total_packets = 0;
    uint16_t flrc_photo_width = 0;
    uint16_t flrc_photo_height = 0;
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

typedef enum {
    MESH_REMOTE_REQ_NODEINFO = 0,
    MESH_REMOTE_REQ_POSITION,
    MESH_REMOTE_REQ_TELEMETRY_DEVICE,
    MESH_REMOTE_REQ_TELEMETRY_ENVIRONMENT,
    MESH_REMOTE_REQ_TRACEROUTE,
    MESH_REMOTE_REQ_NEIGHBORINFO,
} mesh_remote_request_type_t;

typedef struct {
    mesh_remote_request_type_t type = MESH_REMOTE_REQ_NODEINFO;
    uint32_t to_node = 0;
    uint32_t request_id = 0;
} mesh_remote_request_t;

typedef struct {
    mesh_remote_request_type_t type = MESH_REMOTE_REQ_NODEINFO;
    uint32_t to_node = 0;
    uint32_t request_id = 0;
    uint32_t mesh_packet_id = 0;
    uint32_t airtime_ms = 0;
    uint64_t queued_us = 0;
    uint64_t updated_us = 0;
    uint64_t replied_us = 0;
    char state[32] = { 0 };
    char detail[128] = { 0 };
} mesh_remote_request_status_t;

typedef struct {
    std::string message;
    std::vector<uint8_t> payload;
    uint32_t portnum = MESHTASTIC_TEXT_MESSAGE_APP;
    uint32_t channel_index = 0;
    bool has_to_node = false;
    uint32_t to_node = 0;
    bool has_want_ack = false;
    bool want_ack = false;
    bool raw_payload = false;
    bool voice = false;
    bool flrc_voice_after_tx = false;
    std::vector<uint8_t> flrc_voice_payload;
    uint32_t flrc_voice_stream_id = 0;
    uint32_t flrc_voice_duration_ms = 0;
    uint32_t flrc_voice_payload_crc = 0;
    uint16_t flrc_voice_total_packets = 0;
    uint8_t flrc_voice_codec_mode = 0;
    bool flrc_photo_after_tx = false;
    std::vector<uint8_t> flrc_photo_payload;
    uint32_t flrc_photo_stream_id = 0;
    uint32_t flrc_photo_payload_crc = 0;
    uint16_t flrc_photo_total_packets = 0;
    uint16_t flrc_photo_width = 0;
    uint16_t flrc_photo_height = 0;
    std::string summary;
} mesh_send_request_t;

typedef struct {
    uint32_t portnum = 0;
    std::vector<uint8_t> payload;
    uint32_t request_id = 0;
    uint32_t reply_id = 0;
    bool want_response = false;
} mesh_data_proto_t;

static int16_t begin_chip(chip_type_t chip, PhysicalLayer *radio,
                          SX1262 *sx1262, LR2021 *lr2021,
                          const probe_profile_t *profile);
static int start_rx(PhysicalLayer *radio);

typedef struct {
    bool active = false;
    uint32_t to_node = MESHTASTIC_NODENUM_BROADCAST;
    uint32_t from_node = 0;
    uint32_t packet_id = 0;
    uint32_t channel_index = 0;
    uint8_t hop_limit = 3;
    bool want_ack = false;
    mesh_data_proto_t data;
} phoneapi_mesh_tx_t;

typedef struct {
    std::string long_name;
    std::string short_name;
    int hw_model = -1;
    bool has_name = false;
    bool has_public_key = false;
    uint8_t public_key[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
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
    bool valid = false;
    bool has_id = false;
    bool has_latitude = false;
    bool has_longitude = false;
    uint32_t id = 0;
    uint32_t from_node = 0;
    uint32_t expire = 0;
    uint32_t locked_to = 0;
    uint32_t icon = 0;
    uint32_t last_seen_epoch = 0;
    uint64_t last_seen_us = 0;
    int32_t latitude_i = 0;
    int32_t longitude_i = 0;
    char name[32] = {0};
    char description[104] = {0};
} mesh_waypoint_info_t;

typedef struct {
    bool enabled = false;
    bool probed = false;
    bool present = false;
    bool configured = false;
    bool has_fix = false;
    bool first_fix_reported = false;
    int fd = -1;
    int lock_fd = -1;
    size_t line_used = 0;
    uint64_t next_probe_us = 0;
    uint64_t last_rx_us = 0;
    uint64_t last_fix_us = 0;
    uint64_t session_start_us = 0;
    uint64_t last_search_restart_us = 0;
    uint64_t nmea_rx_count = 0;
    uint64_t nmea_valid_count = 0;
    uint64_t nmea_nofix_count = 0;
    uint64_t last_nmea_us = 0;
    uint64_t ttff_ms = 0;
    bool ttff_valid = false;
    bool used_cache_fix = false;
    time_t cache_epoch = 0;
    char line[MESHTASTIC_NRF9151_LINE_MAX];
    char modem_state[32] = "off";
    char gps_state[32] = "off";
    char detail[160] = "-";
    mesh_position_info_t position;
} nrf9151_gnss_state_t;

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
    bool ready = false;
    bool generated = false;
    uint8_t public_key[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
    uint8_t private_key[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
} mesh_pki_identity_t;

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
    {"UA_868", 868.0f, 868.6f, 14, false, REGION_PROFILE_STD, "LONG_FAST", OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH},
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
static uint32_t active_tx_airtime_ms;
static tx_frame_t active_tx_frame;
static bool active_tx_frame_valid;
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
static bool mesh_manual_nodeinfo_requested;
static uint32_t mesh_position_tx_count;
static uint32_t mesh_position_drop_count;
static uint64_t mesh_next_position_us;
static bool mesh_manual_position_requested;
static uint32_t mesh_telemetry_tx_count;
static uint32_t mesh_telemetry_drop_count;
static uint64_t mesh_next_device_telemetry_us;
static uint64_t mesh_next_environment_telemetry_us;
static bool mesh_manual_device_telemetry_requested;
static bool mesh_manual_environment_telemetry_requested;
static uint64_t mesh_bq27220_cache_us;
static uint16_t mesh_bq27220_cached_voltage_mv;
static uint16_t mesh_bq27220_cached_soc;
static int16_t mesh_bq27220_cached_current_ma;
static uint64_t mesh_bq27220_next_fail_log_us;
static nrf9151_gnss_state_t mesh_gnss;
static LR2021 *active_lr2021;
static uint32_t mesh_airtime_channel_ms[MESHTASTIC_AIRTIME_CHANNEL_PERIODS];
static uint32_t mesh_airtime_tx_ms[MESHTASTIC_AIRTIME_TX_PERIODS];
static uint64_t mesh_airtime_channel_slot = UINT64_MAX;
static uint64_t mesh_airtime_tx_slot = UINT64_MAX;
static uint64_t mesh_airtime_tx_total_ms;
static uint64_t mesh_airtime_rx_total_ms;
static mesh_history_entry_t mesh_history[MESHTASTIC_PACKET_HISTORY_SIZE];
static size_t mesh_history_count;
static size_t mesh_history_next;
static mesh_chat_dedup_entry_t mesh_chat_dedup[MESHTASTIC_CHAT_DEDUP_SIZE];
static size_t mesh_chat_dedup_next;
static delayed_tx_t mesh_delayed_tx_queue[MESHTASTIC_DELAYED_TX_QUEUE_SIZE];
static ack_retry_entry_t mesh_ack_retry_queue[MESHTASTIC_ACK_RETRY_QUEUE_SIZE];
static mesh_pki_identity_t mesh_pki_identity;
static char daemon_event_log[MESHTASTIC_EVENT_LOG_LINES][MESHTASTIC_EVENT_LOG_LINE_LEN];
static size_t daemon_event_log_count;
static char daemon_chat_log[MESHTASTIC_CHAT_LOG_LINES][MESHTASTIC_CHAT_LOG_LINE_LEN];
static size_t daemon_chat_log_count;
static mesh_node_entry_t mesh_nodes[MESHTASTIC_NODE_CACHE_SIZE];
static size_t mesh_node_count;
static mesh_waypoint_info_t mesh_waypoints[MESHTASTIC_WAYPOINT_CACHE_SIZE];
static size_t mesh_waypoint_count;
static mesh_remote_request_status_t
    mesh_remote_status_history[MESHTASTIC_REMOTE_STATUS_HISTORY_MAX];
static size_t mesh_remote_status_count;
static size_t mesh_remote_status_next;
static uint32_t mesh_remote_request_next_id = 1U;
static bool mesh_nodedb_dirty;
static bool mesh_nodedb_loaded;
static uint64_t mesh_nodedb_next_save_us;
static uint32_t mesh_nodedb_load_count;
static uint32_t mesh_nodedb_save_count;
static uint32_t mesh_nodedb_save_fail_count;
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
static bool phoneapi_mesh_connected_event;
static uint64_t phoneapi_last_adv_us;
static char phoneapi_pairing_code[16];
static uint64_t phoneapi_pairing_code_us;

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

static void phoneapi_bridge_set_pairing_code(const char *code)
{
    char clean[sizeof(phoneapi_pairing_code)] = { 0 };
    size_t out = 0;

    if(code) {
        for(size_t i = 0; code[i] && out + 1U < sizeof(clean); i++) {
            if(isdigit((unsigned char)code[i])) {
                clean[out++] = code[i];
            }
        }
    }
    pthread_mutex_lock(&phoneapi_state_mutex);
    snprintf(phoneapi_pairing_code, sizeof(phoneapi_pairing_code), "%s",
             clean);
    phoneapi_pairing_code_us = clean[0] ? monotonic_us() : 0ULL;
    pthread_mutex_unlock(&phoneapi_state_mutex);
}

static void phoneapi_bridge_clear_pairing_code(void)
{
    phoneapi_bridge_set_pairing_code(nullptr);
}

static bool phoneapi_bridge_get_pairing_code(char *code, size_t code_len)
{
    bool valid = false;
    uint64_t now = monotonic_us();

    if(!code || code_len == 0U) {
        return false;
    }
    code[0] = '\0';
    pthread_mutex_lock(&phoneapi_state_mutex);
    if(phoneapi_pairing_code[0] &&
       phoneapi_pairing_code_us > 0ULL &&
       now >= phoneapi_pairing_code_us &&
       now - phoneapi_pairing_code_us < 120000000ULL) {
        snprintf(code, code_len, "%s", phoneapi_pairing_code);
        valid = true;
    } else if(phoneapi_pairing_code[0]) {
        phoneapi_pairing_code[0] = '\0';
        phoneapi_pairing_code_us = 0ULL;
    }
    pthread_mutex_unlock(&phoneapi_state_mutex);
    return valid;
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

static bool phoneapi_bridge_has_client(void)
{
    return phoneapi_bridge_get_state(nullptr, 0) == PHONEAPI_BRIDGE_CONNECTED;
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
    if(line.find("CONN=1") != std::string::npos ||
       line.find("MESH_CONN=1") != std::string::npos) {
        return phoneapi_mesh_connected_event;
    }
    return false;
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

static std::string mesh_hex_encode_bytes(const uint8_t *data, size_t len);

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

static bool daemon_chat_update_tx_status(uint32_t packet_id,
                                         const char *status)
{
    char id_text[24];
    char updated[MESHTASTIC_CHAT_LOG_LINE_LEN];

    if(packet_id == 0U || !status || !status[0]) {
        return false;
    }
    snprintf(id_text, sizeof(id_text), "id=0x%08x", packet_id);

    pthread_mutex_lock(&daemon_log_mutex);
    for(size_t i = 0; i < daemon_chat_log_count; i++) {
        char *line = daemon_chat_log[i];
        char *ack;
        char *colon;
        size_t prefix_len;

        if(strncmp(line, "TX ", 3) != 0 || !strstr(line, id_text)) {
            continue;
        }
        ack = strstr(line, "ack=");
        colon = strstr(line, ": ");
        if(!ack || !colon || ack > colon) {
            continue;
        }
        prefix_len = (size_t)(ack - line);
        snprintf(updated, sizeof(updated), "%.*sack=%s%s",
                 (int)prefix_len, line, status, colon);
        snprintf(line, MESHTASTIC_CHAT_LOG_LINE_LEN, "%s", updated);
        pthread_mutex_unlock(&daemon_log_mutex);
        return true;
    }
    pthread_mutex_unlock(&daemon_log_mutex);
    return false;
}

static bool daemon_chat_update_voice_stream_status(uint32_t stream_id,
                                                   const char *state,
                                                   unsigned long elapsed_ms)
{
    char stream_text[24];
    char updated[MESHTASTIC_CHAT_LOG_LINE_LEN];
    char elapsed_text[32] = "";

    if(stream_id == 0U || !state || !state[0]) {
        return false;
    }
    snprintf(stream_text, sizeof(stream_text), "stream=0x%08x", stream_id);
    if(elapsed_ms > 0UL) {
        snprintf(elapsed_text, sizeof(elapsed_text), " elapsed=%lums",
                 elapsed_ms);
    }

    pthread_mutex_lock(&daemon_log_mutex);
    for(size_t i = 0; i < daemon_chat_log_count; i++) {
        char *line = daemon_chat_log[i];
        char *state_pos;
        char *after_state;
        char *file_pos;

        if(strncmp(line, "TX ", 3) != 0 || !strstr(line, " voice ") ||
           !strstr(line, stream_text)) {
            continue;
        }
        state_pos = strstr(line, " state=");
        if(state_pos) {
            after_state = state_pos + 7;
            while(*after_state && !isspace((unsigned char)*after_state)) {
                after_state++;
            }
            snprintf(updated, sizeof(updated), "%.*s state=%s%s%s",
                     (int)(state_pos - line), line, state, elapsed_text,
                     after_state);
        } else {
            file_pos = strstr(line, " file=");
            if(file_pos) {
                snprintf(updated, sizeof(updated), "%.*s state=%s%s%s",
                         (int)(file_pos - line), line, state, elapsed_text,
                         file_pos);
            } else {
                snprintf(updated, sizeof(updated), "%s state=%s%s",
                         line, state, elapsed_text);
            }
        }
        snprintf(line, MESHTASTIC_CHAT_LOG_LINE_LEN, "%s", updated);
        pthread_mutex_unlock(&daemon_log_mutex);
        return true;
    }
    pthread_mutex_unlock(&daemon_log_mutex);
    return false;
}

static bool daemon_chat_update_photo_stream_status(uint32_t stream_id,
                                                   const char *state,
                                                   unsigned long elapsed_ms)
{
    char stream_text[24];
    char updated[MESHTASTIC_CHAT_LOG_LINE_LEN];
    char elapsed_text[32] = "";

    if(stream_id == 0U || !state || !state[0]) {
        return false;
    }
    snprintf(stream_text, sizeof(stream_text), "stream=0x%08x", stream_id);
    if(elapsed_ms > 0UL) {
        snprintf(elapsed_text, sizeof(elapsed_text), " elapsed=%lums",
                 elapsed_ms);
    }

    pthread_mutex_lock(&daemon_log_mutex);
    for(size_t i = 0; i < daemon_chat_log_count; i++) {
        char *line = daemon_chat_log[i];
        char *state_pos;
        char *after_state;
        char *file_pos;

        if(strncmp(line, "TX ", 3) != 0 || !strstr(line, " photo ") ||
           !strstr(line, stream_text)) {
            continue;
        }
        state_pos = strstr(line, " state=");
        if(state_pos) {
            after_state = state_pos + 7;
            while(*after_state && !isspace((unsigned char)*after_state)) {
                after_state++;
            }
            snprintf(updated, sizeof(updated), "%.*s state=%s%s%s",
                     (int)(state_pos - line), line, state, elapsed_text,
                     after_state);
        } else {
            file_pos = strstr(line, " file=");
            if(file_pos) {
                snprintf(updated, sizeof(updated), "%.*s state=%s%s%s",
                         (int)(file_pos - line), line, state, elapsed_text,
                         file_pos);
            } else {
                snprintf(updated, sizeof(updated), "%s state=%s%s",
                         line, state, elapsed_text);
            }
        }
        snprintf(line, MESHTASTIC_CHAT_LOG_LINE_LEN, "%s", updated);
        pthread_mutex_unlock(&daemon_log_mutex);
        return true;
    }
    pthread_mutex_unlock(&daemon_log_mutex);
    return false;
}

static bool mesh_config_dir_ensure(void)
{
    if(mkdir(K230_PHONE_UI_CONFIG_PARENT, 0755) != 0 && errno != EEXIST) {
        daemon_event("Mesh config mkdir %s failed: %s",
                     K230_PHONE_UI_CONFIG_PARENT, strerror(errno));
        return false;
    }
    if(mkdir(K230_PHONE_UI_PREFS_DIR, 0755) != 0 && errno != EEXIST) {
        daemon_event("Mesh config mkdir %s failed: %s",
                     K230_PHONE_UI_PREFS_DIR, strerror(errno));
        return false;
    }
    return true;
}

static uint32_t mesh_now_epoch(void)
{
    time_t now = time(nullptr);

    return now > 0 ? (uint32_t)now : 0U;
}

static void mesh_node_touch_timestamp(mesh_node_entry_t *entry)
{
    if(!entry) {
        return;
    }
    if(entry->last_seen_us == 0ULL) {
        entry->last_seen_us = monotonic_us();
    }
    if(entry->last_seen_epoch == 0U) {
        entry->last_seen_epoch = mesh_now_epoch();
    }
}

static uint32_t mesh_node_last_seen_epoch(const mesh_node_entry_t &node)
{
    uint32_t now_epoch = mesh_now_epoch();
    uint64_t now_us = monotonic_us();
    uint64_t age_s = 0;

    if(node.last_seen_epoch != 0U) {
        return node.last_seen_epoch;
    }
    if(now_epoch == 0U || node.last_seen_us == 0ULL ||
       node.last_seen_us > now_us) {
        return 0U;
    }
    age_s = (now_us - node.last_seen_us) / 1000000ULL;
    if(age_s > now_epoch) {
        return 0U;
    }
    return now_epoch - (uint32_t)age_s;
}

static uint32_t mesh_node_age_seconds(const mesh_node_entry_t &node)
{
    uint32_t now_epoch = mesh_now_epoch();
    uint64_t now_us = monotonic_us();

    if(node.last_seen_epoch != 0U && now_epoch >= node.last_seen_epoch) {
        return now_epoch - node.last_seen_epoch;
    }
    if(node.last_seen_us != 0ULL && node.last_seen_us <= now_us) {
        uint64_t age_s = (now_us - node.last_seen_us) / 1000000ULL;
        return age_s > UINT32_MAX ? UINT32_MAX : (uint32_t)age_s;
    }
    return 0U;
}

static void mesh_nodedb_mark_dirty(void)
{
    uint64_t due_us = monotonic_us() + MESHTASTIC_NODEDB_SAVE_DEBOUNCE_US;

    mesh_nodedb_dirty = true;
    if(mesh_nodedb_next_save_us == 0ULL ||
       mesh_nodedb_next_save_us > due_us) {
        mesh_nodedb_next_save_us = due_us;
    }
}

static mesh_node_entry_t *mesh_node_find(uint32_t node)
{
    if(node == 0U) {
        return nullptr;
    }
    for(size_t i = 0; i < mesh_node_count; i++) {
        if(mesh_nodes[i].node == node) {
            return &mesh_nodes[i];
        }
    }
    return nullptr;
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

static bool mesh_node_remove_by_num(uint32_t node)
{
    mesh_node_entry_t *found;
    size_t i;

    if(node == 0U) {
        return false;
    }
    found = mesh_node_find(node);
    if(!found) {
        return false;
    }
    i = (size_t)(found - mesh_nodes);
    for(size_t j = i + 1U; j < mesh_node_count; j++) {
        mesh_nodes[j - 1U] = mesh_nodes[j];
    }
    if(mesh_node_count > 0U) {
        mesh_node_count--;
        memset(&mesh_nodes[mesh_node_count], 0,
               sizeof(mesh_nodes[mesh_node_count]));
    }
    mesh_nodedb_mark_dirty();
    return true;
}

static bool mesh_node_set_local_flag(uint32_t node, bool favorite_valid,
                                     bool favorite, bool ignored_valid,
                                     bool ignored, bool toggle_muted)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);
    bool changed = false;

    if(!entry) {
        return false;
    }
    mesh_node_touch_timestamp(entry);
    if(favorite_valid && entry->is_favorite != favorite) {
        entry->is_favorite = favorite;
        changed = true;
    }
    if(ignored_valid && entry->is_ignored != ignored) {
        entry->is_ignored = ignored;
        changed = true;
    }
    if(toggle_muted) {
        entry->is_muted = !entry->is_muted;
        changed = true;
    }
    if(changed) {
        mesh_nodedb_mark_dirty();
    }
    return true;
}

static void mesh_nodedb_reset_preserve_favorites(void)
{
    size_t out = 0U;

    for(size_t i = 0; i < mesh_node_count; i++) {
        if(!mesh_nodes[i].is_favorite) {
            continue;
        }
        if(out != i) {
            mesh_nodes[out] = mesh_nodes[i];
        }
        out++;
    }
    for(size_t i = out; i < mesh_node_count; i++) {
        memset(&mesh_nodes[i], 0, sizeof(mesh_nodes[i]));
    }
    mesh_node_count = out;
    mesh_nodedb_mark_dirty();
}

static void mesh_node_seen(uint32_t node, float rssi, float snr)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry) {
        return;
    }
    entry->last_seen_us = monotonic_us();
    entry->last_seen_epoch = mesh_now_epoch();
    entry->rssi_dbm = (int)roundf(rssi);
    entry->snr = snr;
    entry->rx_count++;
    mesh_nodedb_mark_dirty();
}

static bool mesh_node_update_link_info(uint32_t node, uint32_t channel_index,
                                       uint8_t hop_start, uint8_t hop_limit)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);
    uint32_t hops_away = 0U;
    bool changed;

    if(!entry) {
        return false;
    }
    if(hop_start >= hop_limit) {
        hops_away = (uint32_t)(hop_start - hop_limit);
    }
    changed = !entry->has_channel || entry->channel_index != channel_index ||
              !entry->has_hops_away || entry->hops_away != hops_away;
    entry->has_channel = true;
    entry->channel_index = channel_index;
    entry->has_hops_away = true;
    entry->hops_away = hops_away;
    if(changed) {
        mesh_nodedb_mark_dirty();
    }
    return changed;
}

static void mesh_node_update_user(uint32_t node, const mesh_user_info_t &user)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);
    std::string clean_long = mesh_clean_text(user.long_name);
    std::string clean_short = mesh_clean_text(user.short_name);

    if(!entry) {
        return;
    }
    mesh_node_touch_timestamp(entry);
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
    if(user.has_public_key) {
        if(entry->has_public_key &&
           memcmp(entry->public_key, user.public_key,
                  MESHTASTIC_CURVE25519_KEY_LEN) != 0) {
            daemon_event("NodeInfo public key mismatch node=0x%08x keep-existing",
                         node);
        } else if(!entry->has_public_key) {
            memcpy(entry->public_key, user.public_key,
                   MESHTASTIC_CURVE25519_KEY_LEN);
            entry->has_public_key = true;
            daemon_event("NodeInfo public key learned node=0x%08x key=%s",
                         node,
                         mesh_hex_encode_bytes(entry->public_key, 4U).c_str());
        }
    }
    mesh_nodedb_mark_dirty();
}

static void mesh_node_update_position(uint32_t node,
                                      const mesh_position_info_t &position)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry || !position.has_latitude || !position.has_longitude) {
        return;
    }
    mesh_node_touch_timestamp(entry);
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
    mesh_nodedb_mark_dirty();
}

static void mesh_node_update_telemetry(uint32_t node,
                                       const mesh_telemetry_info_t &telemetry)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry) {
        return;
    }
    mesh_node_touch_timestamp(entry);
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
    mesh_nodedb_mark_dirty();
}

static void mesh_node_update_neighbor_info(uint32_t node,
                                           const mesh_neighbor_info_t &info)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);

    if(!entry || !info.has_neighbor_info) {
        return;
    }
    mesh_node_touch_timestamp(entry);
    entry->has_neighbor_info = true;
    entry->neighbor_node_id = info.node_id;
    entry->neighbor_last_sent_by_id = info.last_sent_by_id;
    entry->neighbor_broadcast_interval_secs =
        info.broadcast_interval_secs;
    entry->neighbor_count = info.neighbor_count;
    snprintf(entry->neighbor_summary, sizeof(entry->neighbor_summary), "%s",
             info.summary.empty() ? "-" : info.summary.c_str());
    mesh_nodedb_mark_dirty();
}

static void mesh_node_update_route_info(uint32_t node,
                                        const std::string &summary)
{
    mesh_node_entry_t *entry = mesh_node_get_or_create(node);
    std::string clean = mesh_clean_text(summary);

    if(!entry || clean.empty()) {
        return;
    }
    mesh_node_touch_timestamp(entry);
    entry->has_route_info = true;
    snprintf(entry->route_summary, sizeof(entry->route_summary), "%s",
             clean.c_str());
    mesh_nodedb_mark_dirty();
}

static uint32_t mesh_waypoint_age_seconds(const mesh_waypoint_info_t &wp)
{
    uint32_t now_epoch = mesh_now_epoch();
    uint64_t now_us = monotonic_us();

    if(wp.last_seen_epoch != 0U && now_epoch >= wp.last_seen_epoch) {
        return now_epoch - wp.last_seen_epoch;
    }
    if(wp.last_seen_us != 0ULL && wp.last_seen_us <= now_us) {
        uint64_t age_s = (now_us - wp.last_seen_us) / 1000000ULL;
        return age_s > UINT32_MAX ? UINT32_MAX : (uint32_t)age_s;
    }
    return 0U;
}

static bool mesh_waypoint_is_expired(const mesh_waypoint_info_t &wp,
                                     uint32_t now_epoch)
{
    return wp.valid && wp.expire != 0U && now_epoch != 0U &&
           wp.expire <= now_epoch;
}

static std::string mesh_ipc_token(const char *text, size_t max_len)
{
    std::string out;

    if(!text || !text[0] || max_len == 0U) {
        return "-";
    }
    for(size_t i = 0; text[i] && out.size() < max_len; i++) {
        unsigned char c = (unsigned char)text[i];
        if(c == '\r' || c == '\n' || c == '\t' ||
           isspace((unsigned char)c)) {
            if(!out.empty() && out.back() != '_') {
                out.push_back('_');
            }
        } else if(c >= 32U) {
            out.push_back((char)c);
        }
    }
    while(!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out.empty() ? "-" : out;
}

static void mesh_waypoint_update(uint32_t from_node,
                                 const mesh_waypoint_info_t &waypoint)
{
    size_t slot = MESHTASTIC_WAYPOINT_CACHE_SIZE;
    size_t oldest = 0U;
    uint32_t now_epoch = mesh_now_epoch();
    uint64_t now_us = monotonic_us();

    if(!waypoint.has_latitude || !waypoint.has_longitude) {
        return;
    }
    for(size_t i = 0; i < mesh_waypoint_count; i++) {
        if(!mesh_waypoints[i].valid) {
            slot = i;
            break;
        }
        if(waypoint.has_id && mesh_waypoints[i].has_id &&
           mesh_waypoints[i].id == waypoint.id) {
            slot = i;
            break;
        }
        if(!waypoint.has_id && !mesh_waypoints[i].has_id &&
           mesh_waypoints[i].from_node == from_node) {
            slot = i;
            break;
        }
        if(mesh_waypoints[i].last_seen_us <
           mesh_waypoints[oldest].last_seen_us) {
            oldest = i;
        }
    }
    if(slot == MESHTASTIC_WAYPOINT_CACHE_SIZE) {
        for(size_t i = 0; i < mesh_waypoint_count; i++) {
            if(mesh_waypoint_is_expired(mesh_waypoints[i], now_epoch)) {
                slot = i;
                break;
            }
        }
    }
    if(slot == MESHTASTIC_WAYPOINT_CACHE_SIZE) {
        if(mesh_waypoint_count < MESHTASTIC_WAYPOINT_CACHE_SIZE) {
            slot = mesh_waypoint_count++;
        } else {
            slot = oldest;
        }
    }

    mesh_waypoints[slot] = waypoint;
    mesh_waypoints[slot].valid = true;
    mesh_waypoints[slot].from_node = from_node;
    mesh_waypoints[slot].last_seen_us = now_us;
    mesh_waypoints[slot].last_seen_epoch = now_epoch;
}

static char mesh_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789abcdef";

    return digits[value & 0x0fU];
}

static std::string mesh_hex_encode_text(const char *text, size_t max_len)
{
    std::string out;

    if(!text || !text[0] || max_len == 0U) {
        return "-";
    }
    for(size_t i = 0; i < max_len && text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        out.push_back(mesh_hex_digit(c >> 4U));
        out.push_back(mesh_hex_digit(c));
    }
    return out.empty() ? "-" : out;
}

static int mesh_hex_value(char c)
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

static void mesh_hex_decode_text(const char *hex, char *out, size_t out_len)
{
    size_t pos = 0;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(!hex || !hex[0] || strcmp(hex, "-") == 0) {
        return;
    }
    for(size_t i = 0; hex[i] && hex[i + 1U] && pos + 1U < out_len;
        i += 2U) {
        int hi = mesh_hex_value(hex[i]);
        int lo = mesh_hex_value(hex[i + 1U]);

        if(hi < 0 || lo < 0) {
            break;
        }
        out[pos++] = (char)((hi << 4) | lo);
    }
    out[pos] = '\0';
}

static std::string mesh_hex_encode_bytes(const uint8_t *data, size_t len)
{
    std::string out;

    if(!data || len == 0U) {
        return "-";
    }
    out.reserve(len * 2U);
    for(size_t i = 0; i < len; i++) {
        out.push_back(mesh_hex_digit(data[i] >> 4U));
        out.push_back(mesh_hex_digit(data[i]));
    }
    return out.empty() ? "-" : out;
}

static std::string mesh_sha256_hex_bytes(const uint8_t *data, size_t len)
{
    uint8_t digest[SHA256_DIGEST_LENGTH];

    if(!data || len == 0U) {
        return "-";
    }
    SHA256(data, len, digest);
    return mesh_hex_encode_bytes(digest, sizeof(digest));
}

static std::string mesh_sha256_hex_vector(const std::vector<uint8_t> &data)
{
    return data.empty() ? "-" :
           mesh_sha256_hex_bytes(data.data(), data.size());
}

static bool mesh_hex_decode_bytes(const char *hex, uint8_t *out, size_t len)
{
    size_t hex_len;

    if(!hex || !out || len == 0U) {
        return false;
    }
    if(strncmp(hex, "0x", 2) == 0 || strncmp(hex, "0X", 2) == 0) {
        hex += 2;
    }
    hex_len = strlen(hex);
    if(hex_len != len * 2U) {
        return false;
    }
    for(size_t i = 0; i < len; i++) {
        int hi = mesh_hex_value(hex[i * 2U]);
        int lo = mesh_hex_value(hex[i * 2U + 1U]);

        if(hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4U) | lo);
    }
    return true;
}

static bool mesh_pki_generate_identity(mesh_pki_identity_t *identity)
{
    EVP_PKEY_CTX *ctx;
    EVP_PKEY *pkey = nullptr;
    size_t public_len = MESHTASTIC_CURVE25519_KEY_LEN;
    size_t private_len = MESHTASTIC_CURVE25519_KEY_LEN;
    bool ok = false;

    if(!identity) {
        return false;
    }
    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if(!ctx) {
        return false;
    }
    if(EVP_PKEY_keygen_init(ctx) == 1 &&
       EVP_PKEY_keygen(ctx, &pkey) == 1 &&
       EVP_PKEY_get_raw_public_key(pkey, identity->public_key,
                                   &public_len) == 1 &&
       EVP_PKEY_get_raw_private_key(pkey, identity->private_key,
                                    &private_len) == 1 &&
       public_len == MESHTASTIC_CURVE25519_KEY_LEN &&
       private_len == MESHTASTIC_CURVE25519_KEY_LEN) {
        identity->ready = true;
        identity->generated = true;
        ok = true;
    }
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    if(!ok) {
        memset(identity, 0, sizeof(*identity));
    }
    return ok;
}

static bool mesh_pki_save_identity(const mesh_pki_identity_t &identity)
{
    FILE *fp;
    std::string public_hex;
    std::string private_hex;

    if(!identity.ready || !mesh_config_dir_ensure()) {
        return false;
    }
    public_hex = mesh_hex_encode_bytes(identity.public_key,
                                       sizeof(identity.public_key));
    private_hex = mesh_hex_encode_bytes(identity.private_key,
                                        sizeof(identity.private_key));
    fp = fopen(K230_MESH_IDENTITY_FILE, "w");
    if(!fp) {
        return false;
    }
    fprintf(fp, "# k230 meshtastic identity v1\n");
    fprintf(fp, "public\t%s\n", public_hex.c_str());
    fprintf(fp, "private\t%s\n", private_hex.c_str());
    if(fclose(fp) != 0) {
        unlink(K230_MESH_IDENTITY_FILE);
        return false;
    }
    chmod(K230_MESH_IDENTITY_FILE, 0600);
    return true;
}

static bool mesh_pki_load_identity_file(mesh_pki_identity_t *identity)
{
    FILE *fp;
    char line[256];
    uint8_t public_key[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
    uint8_t private_key[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
    bool have_public = false;
    bool have_private = false;

    if(!identity) {
        return false;
    }
    fp = fopen(K230_MESH_IDENTITY_FILE, "r");
    if(!fp) {
        return false;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *key;
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        if(line[0] == '\0' || line[0] == '#') {
            continue;
        }
        key = line;
        value = strchr(line, '\t');
        if(!value) {
            continue;
        }
        *value++ = '\0';
        if(strcmp(key, "public") == 0) {
            have_public = mesh_hex_decode_bytes(
                value, public_key, sizeof(public_key));
        } else if(strcmp(key, "private") == 0) {
            have_private = mesh_hex_decode_bytes(
                value, private_key, sizeof(private_key));
        }
    }
    fclose(fp);
    if(!have_public || !have_private) {
        return false;
    }
    memset(identity, 0, sizeof(*identity));
    memcpy(identity->public_key, public_key, sizeof(public_key));
    memcpy(identity->private_key, private_key, sizeof(private_key));
    identity->ready = true;
    identity->generated = false;
    return true;
}

static bool mesh_pki_load_or_create_identity(void)
{
    if(mesh_pki_identity.ready) {
        return true;
    }
    if(mesh_pki_load_identity_file(&mesh_pki_identity)) {
        return true;
    }
    if(!mesh_pki_generate_identity(&mesh_pki_identity)) {
        return false;
    }
    return mesh_pki_save_identity(mesh_pki_identity);
}

static bool mesh_pki_public_key_available(void)
{
    return mesh_pki_identity.ready;
}

static bool mesh_parse_u32_text(const char *text, uint32_t *out)
{
    char *end = nullptr;
    unsigned long value;

    if(!text || !text[0] || !out) {
        return false;
    }
    errno = 0;
    value = strtoul(text, &end, 0);
    if(errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool mesh_parse_i32_text(const char *text, int32_t *out)
{
    char *end = nullptr;
    long value;

    if(!text || !text[0] || !out) {
        return false;
    }
    errno = 0;
    value = strtol(text, &end, 0);
    if(errno != 0 || end == text || *end != '\0' ||
       value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

static bool mesh_parse_bool_text(const char *text, bool *out)
{
    uint32_t value;

    if(!out || !mesh_parse_u32_text(text, &value)) {
        return false;
    }
    *out = value != 0U;
    return true;
}

static bool mesh_parse_float_text(const char *text, float *out)
{
    char *end = nullptr;
    float value;

    if(!text || !text[0] || !out) {
        return false;
    }
    errno = 0;
    value = strtof(text, &end);
    if(errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static void mesh_split_tsv(char *line, std::vector<char *> *fields)
{
    char *p;

    if(!line || !fields) {
        return;
    }
    fields->clear();
    p = line;
    while(true) {
        char *tab = strchr(p, '\t');

        fields->push_back(p);
        if(!tab) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
}

static void mesh_loaded_epoch_to_monotonic(mesh_node_entry_t *node)
{
    uint32_t now_epoch = mesh_now_epoch();
    uint64_t now_us = monotonic_us();
    uint64_t age_us = 0;

    if(!node) {
        return;
    }
    if(node->last_seen_epoch == 0U || now_epoch == 0U ||
       node->last_seen_epoch >= now_epoch) {
        node->last_seen_us = now_us;
        return;
    }
    age_us = (uint64_t)(now_epoch - node->last_seen_epoch) * 1000000ULL;
    node->last_seen_us = age_us < now_us ? now_us - age_us : 1ULL;
}

static bool mesh_nodedb_save(void)
{
    FILE *fp;

    if(!mesh_config_dir_ensure()) {
        mesh_nodedb_save_fail_count++;
        return false;
    }
    fp = fopen(K230_MESH_NODEDB_TMP_FILE, "w");
    if(!fp) {
        mesh_nodedb_save_fail_count++;
        daemon_event("NodeDB save open failed: %s", strerror(errno));
        return false;
    }

    fprintf(fp, "# k230 meshtastic nodedb v5\n");
    for(size_t i = 0; i < mesh_node_count; i++) {
        const mesh_node_entry_t &node = mesh_nodes[i];
        std::string long_hex;
        std::string short_hex;
        std::string neighbor_hex;
        std::string route_hex;
        std::string public_key_hex;

        if(node.node == 0U) {
            continue;
        }
        long_hex = mesh_hex_encode_text(node.long_name,
                                        sizeof(node.long_name));
        short_hex = mesh_hex_encode_text(node.short_name,
                                         sizeof(node.short_name));
        neighbor_hex = mesh_hex_encode_text(node.neighbor_summary,
                                            sizeof(node.neighbor_summary));
        route_hex = mesh_hex_encode_text(node.route_summary,
                                         sizeof(node.route_summary));
        public_key_hex = node.has_public_key ?
                         mesh_hex_encode_bytes(node.public_key,
                                               sizeof(node.public_key)) :
                         "-";
        fprintf(fp,
                "v5\t%u\t%u\t%d\t%.3f\t%u\t%s\t%s\t%d\t"
                "%u\t%u\t%u\t%u\t%d\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t"
                "%u\t%u\t%u\t%u\t%u\t%u\t%u\t%.6f\t%.6f\t%.6f\t"
                "%u\t%u\t%u\t%u\t%u\t%u\t%.6f\t%.6f\t%.6f\t%.6f\t%u\t%u\t"
                "%u\t%u\t%u\t%u\t%u\t%s\t%u\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\n",
                node.node, mesh_node_last_seen_epoch(node),
                node.rssi_dbm, node.snr, node.rx_count,
                long_hex.c_str(), short_hex.c_str(), node.hw_model,
                node.has_position ? 1U : 0U,
                node.has_altitude ? 1U : 0U,
                node.has_ground_speed ? 1U : 0U,
                node.has_ground_track ? 1U : 0U,
                node.latitude_i, node.longitude_i, node.altitude_m,
                node.ground_speed_cms, node.ground_track_1e5,
                node.sats_in_view, node.precision_bits,
                node.position_timestamp,
                node.has_device_metrics ? 1U : 0U,
                node.has_battery_level ? 1U : 0U,
                node.has_device_voltage ? 1U : 0U,
                node.has_channel_utilization ? 1U : 0U,
                node.has_air_util_tx ? 1U : 0U,
                node.battery_level, node.uptime_seconds,
                node.device_voltage, node.channel_utilization,
                node.air_util_tx,
                node.has_environment_metrics ? 1U : 0U,
                node.has_temperature ? 1U : 0U,
                node.has_humidity ? 1U : 0U,
                node.has_pressure ? 1U : 0U,
                node.has_environment_voltage ? 1U : 0U,
                node.has_iaq ? 1U : 0U,
                node.temperature_c, node.humidity_percent,
                node.pressure_hpa, node.environment_voltage, node.iaq,
                node.telemetry_timestamp,
                node.has_neighbor_info ? 1U : 0U,
                node.neighbor_node_id, node.neighbor_last_sent_by_id,
                node.neighbor_broadcast_interval_secs,
                node.neighbor_count, neighbor_hex.c_str(),
                node.has_route_info ? 1U : 0U, route_hex.c_str(),
                node.is_favorite ? 1U : 0U,
                node.is_ignored ? 1U : 0U,
                node.is_muted ? 1U : 0U,
                node.has_channel ? 1U : 0U, node.channel_index,
                node.has_hops_away ? 1U : 0U, node.hops_away,
                node.has_public_key ? 1U : 0U, public_key_hex.c_str());
    }
    if(fclose(fp) != 0) {
        unlink(K230_MESH_NODEDB_TMP_FILE);
        mesh_nodedb_save_fail_count++;
        daemon_event("NodeDB save close failed: %s", strerror(errno));
        return false;
    }
    if(rename(K230_MESH_NODEDB_TMP_FILE, K230_MESH_NODEDB_FILE) != 0) {
        unlink(K230_MESH_NODEDB_TMP_FILE);
        mesh_nodedb_save_fail_count++;
        daemon_event("NodeDB save rename failed: %s", strerror(errno));
        return false;
    }

    mesh_nodedb_dirty = false;
    mesh_nodedb_next_save_us = 0ULL;
    mesh_nodedb_save_count++;
    daemon_event("NodeDB saved nodes=%u saves=%u",
                 (unsigned)mesh_node_count, mesh_nodedb_save_count);
    return true;
}

static void mesh_nodedb_maybe_save(uint64_t now_us)
{
    if(!mesh_nodedb_dirty || mesh_nodedb_next_save_us == 0ULL ||
       now_us < mesh_nodedb_next_save_us) {
        return;
    }
    (void)mesh_nodedb_save();
}

static bool mesh_nodedb_load(void)
{
    FILE *fp;
    char line[2048];
    uint32_t loaded = 0;
    uint32_t skipped = 0;

    mesh_nodedb_loaded = true;
    mesh_nodedb_dirty = false;
    mesh_nodedb_next_save_us = 0ULL;

    fp = fopen(K230_MESH_NODEDB_FILE, "r");
    if(!fp) {
        if(errno == ENOENT) {
            daemon_event("NodeDB empty file=%s", K230_MESH_NODEDB_FILE);
            return true;
        }
        daemon_event("NodeDB load open failed: %s", strerror(errno));
        return false;
    }
    while(fgets(line, sizeof(line), fp)) {
        std::vector<char *> fields;
        mesh_node_entry_t tmp;
        mesh_node_entry_t *entry;
        uint32_t value_u32;
        int32_t value_i32;
        float value_f;
        bool value_bool;
        size_t idx = 1;
        bool ok = true;

        line[strcspn(line, "\r\n")] = '\0';
        if(line[0] == '\0' || line[0] == '#') {
            continue;
        }
        mesh_split_tsv(line, &fields);
        if(fields.size() < 49U ||
           (strcmp(fields[0], "v1") != 0 && strcmp(fields[0], "v2") != 0 &&
            strcmp(fields[0], "v3") != 0 && strcmp(fields[0], "v4") != 0 &&
            strcmp(fields[0], "v5") != 0)) {
            skipped++;
            continue;
        }

#define NODEDB_GET_U32(dst) \
        do { \
            if(ok && idx < fields.size() && \
               mesh_parse_u32_text(fields[idx++], &value_u32)) { \
                (dst) = value_u32; \
            } else { \
                ok = false; \
            } \
        } while(0)
#define NODEDB_GET_I32(dst) \
        do { \
            if(ok && idx < fields.size() && \
               mesh_parse_i32_text(fields[idx++], &value_i32)) { \
                (dst) = value_i32; \
            } else { \
                ok = false; \
            } \
        } while(0)
#define NODEDB_GET_BOOL(dst) \
        do { \
            if(ok && idx < fields.size() && \
               mesh_parse_bool_text(fields[idx++], &value_bool)) { \
                (dst) = value_bool; \
            } else { \
                ok = false; \
            } \
        } while(0)
#define NODEDB_GET_FLOAT(dst) \
        do { \
            if(ok && idx < fields.size() && \
               mesh_parse_float_text(fields[idx++], &value_f)) { \
                (dst) = value_f; \
            } else { \
                ok = false; \
            } \
        } while(0)

        tmp.hw_model = -1;
        NODEDB_GET_U32(tmp.node);
        NODEDB_GET_U32(tmp.last_seen_epoch);
        NODEDB_GET_I32(tmp.rssi_dbm);
        NODEDB_GET_FLOAT(tmp.snr);
        NODEDB_GET_U32(tmp.rx_count);
        if(ok && idx < fields.size()) {
            mesh_hex_decode_text(fields[idx++], tmp.long_name,
                                 sizeof(tmp.long_name));
        } else {
            ok = false;
        }
        if(ok && idx < fields.size()) {
            mesh_hex_decode_text(fields[idx++], tmp.short_name,
                                 sizeof(tmp.short_name));
        } else {
            ok = false;
        }
        NODEDB_GET_I32(tmp.hw_model);
        NODEDB_GET_BOOL(tmp.has_position);
        NODEDB_GET_BOOL(tmp.has_altitude);
        NODEDB_GET_BOOL(tmp.has_ground_speed);
        NODEDB_GET_BOOL(tmp.has_ground_track);
        NODEDB_GET_I32(tmp.latitude_i);
        NODEDB_GET_I32(tmp.longitude_i);
        NODEDB_GET_I32(tmp.altitude_m);
        NODEDB_GET_U32(tmp.ground_speed_cms);
        NODEDB_GET_U32(tmp.ground_track_1e5);
        NODEDB_GET_U32(tmp.sats_in_view);
        NODEDB_GET_U32(tmp.precision_bits);
        NODEDB_GET_U32(tmp.position_timestamp);
        NODEDB_GET_BOOL(tmp.has_device_metrics);
        NODEDB_GET_BOOL(tmp.has_battery_level);
        NODEDB_GET_BOOL(tmp.has_device_voltage);
        NODEDB_GET_BOOL(tmp.has_channel_utilization);
        NODEDB_GET_BOOL(tmp.has_air_util_tx);
        NODEDB_GET_U32(tmp.battery_level);
        NODEDB_GET_U32(tmp.uptime_seconds);
        NODEDB_GET_FLOAT(tmp.device_voltage);
        NODEDB_GET_FLOAT(tmp.channel_utilization);
        NODEDB_GET_FLOAT(tmp.air_util_tx);
        NODEDB_GET_BOOL(tmp.has_environment_metrics);
        NODEDB_GET_BOOL(tmp.has_temperature);
        NODEDB_GET_BOOL(tmp.has_humidity);
        NODEDB_GET_BOOL(tmp.has_pressure);
        NODEDB_GET_BOOL(tmp.has_environment_voltage);
        NODEDB_GET_BOOL(tmp.has_iaq);
        NODEDB_GET_FLOAT(tmp.temperature_c);
        NODEDB_GET_FLOAT(tmp.humidity_percent);
        NODEDB_GET_FLOAT(tmp.pressure_hpa);
        NODEDB_GET_FLOAT(tmp.environment_voltage);
        NODEDB_GET_U32(tmp.iaq);
        NODEDB_GET_U32(tmp.telemetry_timestamp);
        NODEDB_GET_BOOL(tmp.has_neighbor_info);
        NODEDB_GET_U32(tmp.neighbor_node_id);
        NODEDB_GET_U32(tmp.neighbor_last_sent_by_id);
        NODEDB_GET_U32(tmp.neighbor_broadcast_interval_secs);
        NODEDB_GET_U32(tmp.neighbor_count);
        if(ok && idx < fields.size()) {
            mesh_hex_decode_text(fields[idx++], tmp.neighbor_summary,
                                 sizeof(tmp.neighbor_summary));
        } else {
            ok = false;
        }
        if(ok && (strcmp(fields[0], "v2") == 0 ||
                  strcmp(fields[0], "v3") == 0 ||
                  strcmp(fields[0], "v4") == 0 ||
                  strcmp(fields[0], "v5") == 0)) {
            NODEDB_GET_BOOL(tmp.has_route_info);
            if(ok && idx < fields.size()) {
                mesh_hex_decode_text(fields[idx++], tmp.route_summary,
                                     sizeof(tmp.route_summary));
            } else {
                ok = false;
            }
        }
        if(ok && (strcmp(fields[0], "v3") == 0 ||
                  strcmp(fields[0], "v4") == 0 ||
                  strcmp(fields[0], "v5") == 0)) {
            NODEDB_GET_BOOL(tmp.is_favorite);
            NODEDB_GET_BOOL(tmp.is_ignored);
            NODEDB_GET_BOOL(tmp.is_muted);
        }
        if(ok && (strcmp(fields[0], "v4") == 0 ||
                  strcmp(fields[0], "v5") == 0)) {
            NODEDB_GET_BOOL(tmp.has_channel);
            NODEDB_GET_U32(tmp.channel_index);
            NODEDB_GET_BOOL(tmp.has_hops_away);
            NODEDB_GET_U32(tmp.hops_away);
        }
        if(ok && strcmp(fields[0], "v5") == 0) {
            NODEDB_GET_BOOL(tmp.has_public_key);
            if(ok && idx < fields.size()) {
                if(tmp.has_public_key) {
                    ok = mesh_hex_decode_bytes(fields[idx], tmp.public_key,
                                               sizeof(tmp.public_key));
                }
                idx++;
            } else {
                ok = false;
            }
        }

#undef NODEDB_GET_U32
#undef NODEDB_GET_I32
#undef NODEDB_GET_BOOL
#undef NODEDB_GET_FLOAT

        if(!ok || tmp.node == 0U) {
            skipped++;
            continue;
        }
        mesh_loaded_epoch_to_monotonic(&tmp);
        entry = mesh_node_get_or_create(tmp.node);
        if(!entry) {
            skipped++;
            continue;
        }
        *entry = tmp;
        loaded++;
    }
    fclose(fp);
    mesh_nodedb_load_count = loaded;
    mesh_nodedb_dirty = false;
    mesh_nodedb_next_save_us = 0ULL;
    daemon_event("NodeDB loaded nodes=%u skipped=%u file=%s",
                 loaded, skipped, K230_MESH_NODEDB_FILE);
    return true;
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

static void mesh_airtime_rotate(uint64_t now_us)
{
    uint64_t channel_slot = now_us / MESHTASTIC_AIRTIME_CHANNEL_PERIOD_US;
    uint64_t tx_slot = now_us / MESHTASTIC_AIRTIME_TX_PERIOD_US;

    if(mesh_airtime_channel_slot == UINT64_MAX) {
        mesh_airtime_channel_slot = channel_slot;
        memset(mesh_airtime_channel_ms, 0, sizeof(mesh_airtime_channel_ms));
    } else if(channel_slot != mesh_airtime_channel_slot) {
        uint64_t delta = channel_slot - mesh_airtime_channel_slot;
        if(delta >= MESHTASTIC_AIRTIME_CHANNEL_PERIODS) {
            memset(mesh_airtime_channel_ms, 0, sizeof(mesh_airtime_channel_ms));
        } else {
            for(uint64_t i = 1; i <= delta; i++) {
                mesh_airtime_channel_ms[
                    (mesh_airtime_channel_slot + i) %
                    MESHTASTIC_AIRTIME_CHANNEL_PERIODS] = 0U;
            }
        }
        mesh_airtime_channel_slot = channel_slot;
    }

    if(mesh_airtime_tx_slot == UINT64_MAX) {
        mesh_airtime_tx_slot = tx_slot;
        memset(mesh_airtime_tx_ms, 0, sizeof(mesh_airtime_tx_ms));
    } else if(tx_slot != mesh_airtime_tx_slot) {
        uint64_t delta = tx_slot - mesh_airtime_tx_slot;
        if(delta >= MESHTASTIC_AIRTIME_TX_PERIODS) {
            memset(mesh_airtime_tx_ms, 0, sizeof(mesh_airtime_tx_ms));
        } else {
            for(uint64_t i = 1; i <= delta; i++) {
                mesh_airtime_tx_ms[
                    (mesh_airtime_tx_slot + i) %
                    MESHTASTIC_AIRTIME_TX_PERIODS] = 0U;
            }
        }
        mesh_airtime_tx_slot = tx_slot;
    }
}

static uint32_t mesh_radio_airtime_ms(PhysicalLayer *radio, size_t len)
{
    RadioLibTime_t airtime_us;

    if(!radio || len == 0U) {
        return 0U;
    }
    airtime_us = radio->getTimeOnAir(len);
    if(airtime_us <= 0) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)airtime_us + 999ULL) / 1000ULL);
}

static void mesh_airtime_log_tx(uint32_t airtime_ms)
{
    uint64_t now = monotonic_us();
    size_t channel_idx;
    size_t tx_idx;

    if(airtime_ms == 0U) {
        return;
    }
    mesh_airtime_rotate(now);
    channel_idx = (size_t)(mesh_airtime_channel_slot %
                           MESHTASTIC_AIRTIME_CHANNEL_PERIODS);
    tx_idx = (size_t)(mesh_airtime_tx_slot % MESHTASTIC_AIRTIME_TX_PERIODS);
    mesh_airtime_channel_ms[channel_idx] += airtime_ms;
    mesh_airtime_tx_ms[tx_idx] += airtime_ms;
    mesh_airtime_tx_total_ms += airtime_ms;
}

static void mesh_airtime_log_rx(uint32_t airtime_ms)
{
    uint64_t now = monotonic_us();
    size_t channel_idx;

    if(airtime_ms == 0U) {
        return;
    }
    mesh_airtime_rotate(now);
    channel_idx = (size_t)(mesh_airtime_channel_slot %
                           MESHTASTIC_AIRTIME_CHANNEL_PERIODS);
    mesh_airtime_channel_ms[channel_idx] += airtime_ms;
    mesh_airtime_rx_total_ms += airtime_ms;
}

static float mesh_airtime_channel_util_percent(void)
{
    uint64_t sum = 0ULL;

    mesh_airtime_rotate(monotonic_us());
    for(size_t i = 0; i < MESHTASTIC_AIRTIME_CHANNEL_PERIODS; i++) {
        sum += mesh_airtime_channel_ms[i];
    }
    return ((float)sum /
            (float)(MESHTASTIC_AIRTIME_CHANNEL_PERIODS * 10U * 1000U)) *
           100.0f;
}

static float mesh_airtime_tx_util_percent(void)
{
    uint64_t sum = 0ULL;

    mesh_airtime_rotate(monotonic_us());
    for(size_t i = 0; i < MESHTASTIC_AIRTIME_TX_PERIODS; i++) {
        sum += mesh_airtime_tx_ms[i];
    }
    return ((float)sum /
            (float)(MESHTASTIC_AIRTIME_TX_PERIODS * 60U * 1000U)) *
           100.0f;
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

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc ^= 0xFFFFFFFFU;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8U; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
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

static float meshtastic_region_duty_cycle_percent(const std::string &region)
{
    std::string name = normalize_token(region.c_str());

    if(name == "EU433" || name == "EU868" || name == "TH" ||
       name == "UA433") {
        return 10.0f;
    }
    if(name == "UA868") {
        return 1.0f;
    }
    return 100.0f;
}

static float meshtastic_region_duty_cycle_percent(
    const probe_options_t &opts)
{
    std::string region = opts.resolved_region.empty() ? opts.region :
                                                    opts.resolved_region;

    if(region.empty()) {
        region = MESHTASTIC_DEFAULT_REGION;
    }
    return meshtastic_region_duty_cycle_percent(region);
}

static bool mesh_airtime_allowed(const probe_options_t &opts,
                                 uint32_t estimated_airtime_ms,
                                 bool polite, char *errbuf,
                                 size_t errbuf_len)
{
    float channel_util = mesh_airtime_channel_util_percent();
    float tx_util = mesh_airtime_tx_util_percent();
    float duty_cycle = meshtastic_region_duty_cycle_percent(opts);
    float channel_limit = polite ? MESHTASTIC_AIRTIME_POLITE_CHANNEL_UTIL_PERCENT :
                                  MESHTASTIC_AIRTIME_MAX_CHANNEL_UTIL_PERCENT;
    float tx_limit = duty_cycle < 100.0f ?
        duty_cycle * (polite ? MESHTASTIC_AIRTIME_POLITE_DUTY_CYCLE_RATIO :
                               1.0f) : 100.0f;
    float channel_projected = channel_util;
    float tx_projected = tx_util;

    if(estimated_airtime_ms > 0U) {
        channel_projected +=
            ((float)estimated_airtime_ms /
             (float)(MESHTASTIC_AIRTIME_CHANNEL_PERIODS * 10U * 1000U)) *
            100.0f;
        tx_projected +=
            ((float)estimated_airtime_ms /
             (float)(MESHTASTIC_AIRTIME_TX_PERIODS * 60U * 1000U)) *
            100.0f;
    }

    if(channel_util >= channel_limit) {
        snprintf(errbuf, errbuf_len,
                 "channel-busy ch_util=%.1f limit=%.1f",
                 channel_util, channel_limit);
        return false;
    }
    if(channel_projected >= channel_limit) {
        snprintf(errbuf, errbuf_len,
                 "channel-budget ch_util=%.1f projected=%.1f limit=%.1f airtime_ms=%u",
                 channel_util, channel_projected, channel_limit,
                 estimated_airtime_ms);
        return false;
    }
    if(tx_util >= tx_limit) {
        snprintf(errbuf, errbuf_len,
                 "duty-cycle air_tx=%.2f limit=%.2f duty=%.1f",
                 tx_util, tx_limit, duty_cycle);
        return false;
    }
    if(tx_projected >= tx_limit) {
        snprintf(errbuf, errbuf_len,
                 "duty-budget air_tx=%.2f projected=%.2f limit=%.2f duty=%.1f airtime_ms=%u",
                 tx_util, tx_projected, tx_limit, duty_cycle,
                 estimated_airtime_ms);
        return false;
    }
    if(errbuf && errbuf_len > 0U) {
        errbuf[0] = '\0';
    }
    return true;
}

static bool mesh_frame_airtime_allowed(PhysicalLayer *radio,
                                       const probe_options_t &opts,
                                       const tx_frame_t &frame,
                                       bool polite,
                                       char *errbuf,
                                       size_t errbuf_len,
                                       uint32_t *airtime_ms)
{
    uint32_t estimate = mesh_radio_airtime_ms(radio, frame.bytes.size());

    if(airtime_ms) {
        *airtime_ms = estimate;
    }
    return mesh_airtime_allowed(opts, estimate, polite, errbuf, errbuf_len);
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

static std::string mesh_psk_bytes_to_text(const std::vector<uint8_t> &psk)
{
    char buf[80];
    static const char hex[] = "0123456789abcdef";

    if(psk.empty() || (psk.size() == 1U && psk[0] == 0U)) {
        return "none";
    }
    if(psk.size() == 1U && psk[0] >= 1U && psk[0] <= 10U) {
        std::vector<uint8_t> key(default_psk,
                                 default_psk + sizeof(default_psk));
        if(psk[0] == 1U) {
            return "default";
        }
        key[key.size() - 1U] =
            (uint8_t)(key[key.size() - 1U] + (psk[0] - 1U));
        return mesh_psk_bytes_to_text(key);
    }
    if(psk.size() == sizeof(default_psk) &&
       memcmp(psk.data(), default_psk, sizeof(default_psk)) == 0) {
        return "default";
    }
    if(psk.size() != 16U && psk.size() != 32U) {
        return "default";
    }
    if(psk.size() * 2U + 3U > sizeof(buf)) {
        return "default";
    }
    buf[0] = '0';
    buf[1] = 'x';
    for(size_t i = 0; i < psk.size(); i++) {
        buf[2U + i * 2U] = hex[(psk[i] >> 4U) & 0x0fU];
        buf[3U + i * 2U] = hex[psk[i] & 0x0fU];
    }
    buf[2U + psk.size() * 2U] = '\0';
    return std::string(buf);
}

static std::string mesh_channel_slot_name(const probe_options_t &opts,
                                          uint32_t index)
{
    if(index < MESHTASTIC_PHONEAPI_MAX_CHANNELS &&
       opts.channels[index].configured &&
       !opts.channels[index].name.empty()) {
        return opts.channels[index].name;
    }
    if(index == opts.primary_channel_index) {
        return effective_mesh_channel_name(opts);
    }
    return std::string();
}

static std::string mesh_channel_slot_psk(const probe_options_t &opts,
                                         uint32_t index)
{
    if(index < MESHTASTIC_PHONEAPI_MAX_CHANNELS &&
       opts.channels[index].configured &&
       !opts.channels[index].psk.empty()) {
        return opts.channels[index].psk;
    }
    if(index == opts.primary_channel_index) {
        return opts.psk.empty() ? std::string("default") : opts.psk;
    }
    return std::string("default");
}

static uint32_t mesh_channel_slot_role(const probe_options_t &opts,
                                       uint32_t index)
{
    if(index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        return MESHTASTIC_CHANNEL_ROLE_DISABLED;
    }
    if(opts.channels[index].configured) {
        return opts.channels[index].role;
    }
    return index == opts.primary_channel_index ?
           MESHTASTIC_CHANNEL_ROLE_PRIMARY :
           MESHTASTIC_CHANNEL_ROLE_DISABLED;
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

static uint32_t mesh_tx_channel_slot_index(const probe_options_t &opts,
                                           uint32_t requested_index)
{
    if(requested_index == 0U &&
       opts.primary_channel_index < MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        return opts.primary_channel_index;
    }
    return requested_index;
}

static bool mesh_resolve_tx_channel(const probe_options_t &opts,
                                    uint32_t requested_index,
                                    std::string *channel_name,
                                    std::string *psk,
                                    std::vector<uint8_t> *key,
                                    uint8_t *hash)
{
    uint32_t index = mesh_tx_channel_slot_index(opts, requested_index);
    uint32_t role;
    std::string local_name;
    std::string local_psk;
    std::vector<uint8_t> local_key;

    if(index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        return false;
    }
    role = mesh_channel_slot_role(opts, index);
    if(role == MESHTASTIC_CHANNEL_ROLE_DISABLED) {
        return false;
    }
    local_name = mesh_channel_slot_name(opts, index);
    local_psk = mesh_channel_slot_psk(opts, index);
    if(!parse_psk(local_psk, &local_key)) {
        return false;
    }
    if(channel_name) {
        *channel_name = local_name;
    }
    if(psk) {
        *psk = local_psk;
    }
    if(key) {
        *key = local_key;
    }
    if(hash) {
        *hash = mesh_channel_hash(local_name, local_key);
    }
    return true;
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
                              std::vector<uint8_t> *out,
                              bool want_response = false,
                              uint32_t dest = 0U,
                              uint32_t source = 0U)
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
    if(want_response) {
        out->push_back(0x18U);
        append_varint(out, 1U);
    }
    if(dest != 0U) {
        out->push_back(0x25U);
        append_fixed32(out, dest);
    }
    if(source != 0U) {
        out->push_back(0x2dU);
        append_fixed32(out, source);
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
    append_varint(out, MESHTASTIC_HW_MODEL_NRF52840_PCA10059);
    if(mesh_pki_public_key_available()) {
        append_varint(out, (8U << 3U) | 2U);
        append_varint(out, MESHTASTIC_CURVE25519_KEY_LEN);
        out->insert(out->end(), mesh_pki_identity.public_key,
                    mesh_pki_identity.public_key +
                    MESHTASTIC_CURVE25519_KEY_LEN);
    }
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

static uint32_t mesh_zigzag32_encode(int32_t value)
{
    return ((uint32_t)value << 1U) ^ (uint32_t)(value >> 31);
}

static int32_t mesh_zigzag32_decode(uint32_t value)
{
    return (int32_t)((value >> 1U) ^ (uint32_t)-(int32_t)(value & 1U));
}

static void append_sint32_field(std::vector<uint8_t> *out, uint32_t field,
                                int32_t value)
{
    if(!out) {
        return;
    }
    append_varint(out, (field << 3U) | 0U);
    append_varint(out, mesh_zigzag32_encode(value));
}

static void append_sfixed32_field(std::vector<uint8_t> *out, uint32_t field,
                                  int32_t value)
{
    if(!out) {
        return;
    }
    append_varint(out, (field << 3U) | 5U);
    append_fixed32(out, (uint32_t)value);
}

static void append_fixed32_field(std::vector<uint8_t> *out, uint32_t field,
                                 uint32_t value)
{
    if(!out) {
        return;
    }
    append_varint(out, (field << 3U) | 5U);
    append_fixed32(out, value);
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

static int mesh_read_first_line(const char *path, char *buf, size_t len)
{
    FILE *fp;

    if(!path || !buf || len == 0U) {
        return -1;
    }
    fp = fopen(path, "r");
    if(!fp) {
        return -1;
    }
    if(!fgets(buf, len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int mesh_read_scaled_double(const char *path, double scale,
                                   double *out)
{
    char line[64];
    char *endp = nullptr;
    double value;

    if(!out || mesh_read_first_line(path, line, sizeof(line)) != 0 ||
       scale == 0.0) {
        return -1;
    }
    errno = 0;
    value = strtod(line, &endp);
    if(errno != 0 || endp == line || !isfinite(value)) {
        return -1;
    }
    *out = value / scale;
    return 0;
}

static bool mesh_read_power_supply_attr(const char *supply, const char *attr,
                                        double scale, double *out)
{
    char path[128];

    if(!supply || !attr || !out) {
        return false;
    }
    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", supply,
             attr);
    return mesh_read_scaled_double(path, scale, out) == 0;
}

static bool mesh_read_power_supply_online(void)
{
    static const char *const supplies[] = {
        "usb", "USB", "ac", "AC", "bq25890-charger", "bq25896-charger"
    };
    double online = 0.0;

    for(size_t i = 0; i < ARRAY_SIZE(supplies); i++) {
        if(mesh_read_power_supply_attr(supplies[i], "online", 1.0,
                                       &online)) {
            return online > 0.5;
        }
    }
    return false;
}

static bool mesh_read_power_supply_device_metrics(
    mesh_telemetry_info_t *telemetry)
{
    static const char *const supplies[] = {
        "battery", "Battery", "BAT0", "bq27220-battery", "bq27220"
    };
    bool found = false;

    if(!telemetry) {
        return false;
    }
    for(size_t i = 0; i < ARRAY_SIZE(supplies); i++) {
        double value = 0.0;

        if(!telemetry->has_battery_level &&
           mesh_read_power_supply_attr(supplies[i], "capacity", 1.0,
                                       &value) &&
           value >= 0.0 && value <= 100.0) {
            telemetry->has_battery_level = true;
            telemetry->battery_level = (uint32_t)llround(value);
            found = true;
        }
        if(!telemetry->has_device_voltage &&
           (mesh_read_power_supply_attr(supplies[i], "voltage_now",
                                        1000000.0, &value) ||
            mesh_read_power_supply_attr(supplies[i], "voltage_avg",
                                        1000000.0, &value)) &&
           value > 0.0 && value < 20.0) {
            telemetry->has_device_voltage = true;
            telemetry->device_voltage = (float)value;
            found = true;
        }
        if(telemetry->has_battery_level && telemetry->has_device_voltage) {
            break;
        }
    }
    return found;
}

static bool mesh_read_aht20_hwmon(mesh_telemetry_info_t *telemetry)
{
    char name_path[96];
    char name[64];

    if(!telemetry) {
        return false;
    }
    for(int i = 0; i < 16; i++) {
        char temp_path[96];
        char hum_path[96];
        double temp = 0.0;
        double hum = 0.0;

        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/hwmon%d/name",
                 i);
        if(mesh_read_first_line(name_path, name, sizeof(name)) != 0) {
            continue;
        }
        if(strcmp(name, "aht10") != 0 && strcmp(name, "aht20") != 0) {
            continue;
        }
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/hwmon/hwmon%d/temp1_input", i);
        snprintf(hum_path, sizeof(hum_path),
                 "/sys/class/hwmon/hwmon%d/humidity1_input", i);
        if(mesh_read_scaled_double(temp_path, 1000.0, &temp) == 0 &&
           mesh_read_scaled_double(hum_path, 1000.0, &hum) == 0 &&
           temp > -40.0 && temp < 125.0 && hum >= 0.0 && hum <= 100.0) {
            telemetry->has_temperature = true;
            telemetry->temperature_c = (float)temp;
            telemetry->has_humidity = true;
            telemetry->humidity_percent = (float)hum;
            return true;
        }
    }
    return false;
}

static int mesh_aht20_select_addr(int fd)
{
    if(ioctl(fd, I2C_SLAVE, MESHTASTIC_AHT20_ADDR) == 0) {
        return 0;
    }
    return ioctl(fd, I2C_SLAVE_FORCE, MESHTASTIC_AHT20_ADDR);
}

static int mesh_aht20_prepare(int fd)
{
    unsigned char status = 0;
    unsigned char init_cmd[3] = {0xBE, 0x08, 0x00};

    if(read(fd, &status, 1) == 1 &&
       (status & MESHTASTIC_AHT20_STATUS_CALIBRATED)) {
        return 0;
    }
    if(write(fd, init_cmd, sizeof(init_cmd)) != (ssize_t)sizeof(init_cmd)) {
        return -1;
    }
    usleep(10000);
    return 0;
}

static bool mesh_read_aht20_i2c(mesh_telemetry_info_t *telemetry)
{
    int fd;
    unsigned char measure_cmd[3] = {0xAC, 0x33, 0x00};
    unsigned char data[7];
    uint32_t raw_hum;
    uint32_t raw_temp;
    ssize_t got;
    double temp;
    double hum;

    if(!telemetry) {
        return false;
    }
    fd = open(MESHTASTIC_AHT20_I2C_DEV, O_RDWR | O_CLOEXEC);
    if(fd < 0) {
        return false;
    }
    if(mesh_aht20_select_addr(fd) != 0 || mesh_aht20_prepare(fd) != 0 ||
       write(fd, measure_cmd, sizeof(measure_cmd)) !=
           (ssize_t)sizeof(measure_cmd)) {
        close(fd);
        return false;
    }
    usleep(90000);
    got = read(fd, data, sizeof(data));
    if(got >= 1 && (data[0] & MESHTASTIC_AHT20_STATUS_BUSY)) {
        usleep(20000);
        got = read(fd, data, sizeof(data));
    }
    close(fd);
    if(got < 6 || (data[0] & MESHTASTIC_AHT20_STATUS_BUSY)) {
        return false;
    }

    raw_hum = ((uint32_t)data[1] << 12) |
              ((uint32_t)data[2] << 4) |
              ((uint32_t)data[3] >> 4);
    raw_temp = (((uint32_t)data[3] & 0x0FU) << 16) |
               ((uint32_t)data[4] << 8) |
               (uint32_t)data[5];
    hum = (double)raw_hum * 100.0 / 1048576.0;
    temp = (double)raw_temp * 200.0 / 1048576.0 - 50.0;
    if(temp <= -40.0 || temp >= 125.0 || hum < 0.0 || hum > 100.0) {
        return false;
    }
    telemetry->has_temperature = true;
    telemetry->temperature_c = (float)temp;
    telemetry->has_humidity = true;
    telemetry->humidity_percent = (float)hum;
    return true;
}

typedef struct {
    struct gpiod_chip *chip = nullptr;
    struct gpiod_line_request *request = nullptr;
} mesh_gpio_i2c_t;

static uint32_t mesh_i2c4_iomux_value(unsigned int sel)
{
    return (sel << 11U) | (1U << 8U) | (1U << 7U) | (8U << 1U) | 1U;
}

static int mesh_i2c4_iomux_set(unsigned int sel)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if(fd < 0) {
        return -1;
    }
    map = mmap(NULL, MESHTASTIC_I2C4_IOMUX_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, MESHTASTIC_I2C4_IOMUX_BASE);
    close(fd);
    if(map == MAP_FAILED) {
        return -1;
    }
    regs = (volatile uint32_t *)map;
    regs[MESHTASTIC_I2C4_IOMUX_IO46_OFFSET / 4U] =
        mesh_i2c4_iomux_value(sel);
    regs[MESHTASTIC_I2C4_IOMUX_IO47_OFFSET / 4U] =
        mesh_i2c4_iomux_value(sel);
    munmap(map, MESHTASTIC_I2C4_IOMUX_SIZE);
    return 0;
}

static void mesh_gpio_i2c_delay(void)
{
    usleep(8);
}

static int mesh_gpio_i2c_request(mesh_gpio_i2c_t *bus)
{
    struct gpiod_line_settings *settings = nullptr;
    struct gpiod_line_config *line_config = nullptr;
    struct gpiod_request_config *request_config = nullptr;
    unsigned int offsets[2] = {
        MESHTASTIC_I2C4_GPIO_SCL_OFFSET,
        MESHTASTIC_I2C4_GPIO_SDA_OFFSET
    };
    int ret = -1;

    if(!bus) {
        return -1;
    }
    bus->chip = nullptr;
    bus->request = nullptr;
    bus->chip = gpiod_chip_open(MESHTASTIC_I2C4_GPIO_CHIP);
    if(!bus->chip) {
        return -1;
    }
    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if(!settings || !line_config || !request_config) {
        goto out;
    }
    gpiod_line_settings_set_direction(settings,
                                      GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings,
                                         GPIOD_LINE_VALUE_ACTIVE);
    gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_OPEN_DRAIN);
    gpiod_request_config_set_consumer(request_config,
                                      "k230-meshtastic-i2c4");
    if(gpiod_line_config_add_line_settings(line_config, offsets, 2,
                                           settings) != 0) {
        goto out;
    }
    bus->request = gpiod_chip_request_lines(bus->chip, request_config,
                                            line_config);
    if(!bus->request) {
        goto out;
    }
    ret = 0;

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
    if(ret != 0) {
        if(bus->request) {
            gpiod_line_request_release(bus->request);
            bus->request = nullptr;
        }
        if(bus->chip) {
            gpiod_chip_close(bus->chip);
            bus->chip = nullptr;
        }
    }
    return ret;
}

static void mesh_gpio_i2c_release(mesh_gpio_i2c_t *bus)
{
    if(!bus) {
        return;
    }
    if(bus->request) {
        gpiod_line_request_release(bus->request);
        bus->request = nullptr;
    }
    if(bus->chip) {
        gpiod_chip_close(bus->chip);
        bus->chip = nullptr;
    }
}

static int mesh_gpio_i2c_set(mesh_gpio_i2c_t *bus, unsigned int offset,
                             int high)
{
    if(!bus || !bus->request) {
        return -1;
    }
    return gpiod_line_request_set_value(
               bus->request, offset,
               high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

static int mesh_gpio_i2c_get(mesh_gpio_i2c_t *bus, unsigned int offset)
{
    enum gpiod_line_value value;

    if(!bus || !bus->request) {
        return -1;
    }
    value = gpiod_line_request_get_value(bus->request, offset);
    if(value < 0) {
        return -1;
    }
    return value == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0;
}

static void mesh_gpio_i2c_scl(mesh_gpio_i2c_t *bus, int high)
{
    (void)mesh_gpio_i2c_set(bus, MESHTASTIC_I2C4_GPIO_SCL_OFFSET, high);
    mesh_gpio_i2c_delay();
}

static void mesh_gpio_i2c_sda(mesh_gpio_i2c_t *bus, int high)
{
    (void)mesh_gpio_i2c_set(bus, MESHTASTIC_I2C4_GPIO_SDA_OFFSET, high);
    mesh_gpio_i2c_delay();
}

static int mesh_gpio_i2c_read_sda(mesh_gpio_i2c_t *bus)
{
    mesh_gpio_i2c_delay();
    return mesh_gpio_i2c_get(bus, MESHTASTIC_I2C4_GPIO_SDA_OFFSET);
}

static void mesh_gpio_i2c_start(mesh_gpio_i2c_t *bus)
{
    mesh_gpio_i2c_sda(bus, 1);
    mesh_gpio_i2c_scl(bus, 1);
    mesh_gpio_i2c_sda(bus, 0);
    mesh_gpio_i2c_scl(bus, 0);
}

static void mesh_gpio_i2c_stop(mesh_gpio_i2c_t *bus)
{
    mesh_gpio_i2c_sda(bus, 0);
    mesh_gpio_i2c_scl(bus, 1);
    mesh_gpio_i2c_sda(bus, 1);
}

static int mesh_gpio_i2c_write_byte(mesh_gpio_i2c_t *bus, uint8_t value)
{
    for(int bit = 7; bit >= 0; bit--) {
        mesh_gpio_i2c_sda(bus, (value >> bit) & 1U);
        mesh_gpio_i2c_scl(bus, 1);
        mesh_gpio_i2c_scl(bus, 0);
    }
    mesh_gpio_i2c_sda(bus, 1);
    mesh_gpio_i2c_scl(bus, 1);
    int ack = mesh_gpio_i2c_read_sda(bus) == 0;
    mesh_gpio_i2c_scl(bus, 0);
    return ack ? 0 : -1;
}

static uint8_t mesh_gpio_i2c_read_byte(mesh_gpio_i2c_t *bus, int ack)
{
    uint8_t value = 0;

    mesh_gpio_i2c_sda(bus, 1);
    for(int bit = 7; bit >= 0; bit--) {
        mesh_gpio_i2c_scl(bus, 1);
        if(mesh_gpio_i2c_read_sda(bus) > 0) {
            value |= (uint8_t)(1U << bit);
        }
        mesh_gpio_i2c_scl(bus, 0);
    }
    mesh_gpio_i2c_sda(bus, ack ? 0 : 1);
    mesh_gpio_i2c_scl(bus, 1);
    mesh_gpio_i2c_scl(bus, 0);
    mesh_gpio_i2c_sda(bus, 1);
    return value;
}

static int mesh_gpio_i2c_begin(mesh_gpio_i2c_t *bus)
{
    if(mesh_i2c4_iomux_set(0) != 0) {
        return -1;
    }
    if(mesh_gpio_i2c_request(bus) != 0) {
        (void)mesh_i2c4_iomux_set(3);
        return -1;
    }
    return 0;
}

static void mesh_gpio_i2c_end(mesh_gpio_i2c_t *bus)
{
    mesh_gpio_i2c_release(bus);
    (void)mesh_i2c4_iomux_set(3);
}

static int mesh_gpio_i2c_read_block(uint8_t addr, uint8_t reg,
                                    uint8_t *buf, size_t len)
{
    mesh_gpio_i2c_t bus;
    int ret = -1;

    if(!buf || len == 0U || mesh_gpio_i2c_begin(&bus) != 0) {
        return -1;
    }
    mesh_gpio_i2c_start(&bus);
    if(mesh_gpio_i2c_write_byte(&bus, (uint8_t)(addr << 1U)) != 0) {
        goto out_stop;
    }
    if(mesh_gpio_i2c_write_byte(&bus, reg) != 0) {
        goto out_stop;
    }
    mesh_gpio_i2c_start(&bus);
    if(mesh_gpio_i2c_write_byte(&bus, (uint8_t)((addr << 1U) | 1U)) != 0) {
        goto out_stop;
    }
    for(size_t i = 0; i < len; i++) {
        buf[i] = mesh_gpio_i2c_read_byte(&bus, i + 1U < len);
    }
    ret = 0;

out_stop:
    mesh_gpio_i2c_stop(&bus);
    mesh_gpio_i2c_end(&bus);
    return ret;
}

static int mesh_gpio_i2c_read_word_le(uint8_t addr, uint8_t reg,
                                      uint16_t *value)
{
    uint8_t buf[2];

    if(!value || mesh_gpio_i2c_read_block(addr, reg, buf, sizeof(buf)) != 0) {
        return -1;
    }
    *value = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8U);
    return 0;
}

static bool mesh_read_bq27220_device_metrics(mesh_telemetry_info_t *telemetry)
{
    uint16_t voltage = 0;
    uint16_t soc = 0;
    uint16_t current = 0;
    uint64_t now_us;
    int soc_rc;
    int current_rc;
    bool found = false;

    if(!telemetry ||
       mesh_gpio_i2c_read_word_le(MESHTASTIC_BQ27220_ADDR,
                                  MESHTASTIC_BQ27220_REG_VOLTAGE,
                                  &voltage) != 0) {
        now_us = monotonic_us();
        if(now_us >= mesh_bq27220_next_fail_log_us) {
            daemon_event("Telemetry BQ27220 read failed");
            mesh_bq27220_next_fail_log_us = now_us + 60000000ULL;
        }
        return false;
    }

    soc_rc = mesh_gpio_i2c_read_word_le(MESHTASTIC_BQ27220_ADDR,
                                        MESHTASTIC_BQ27220_REG_SOC, &soc);
    current_rc = mesh_gpio_i2c_read_word_le(MESHTASTIC_BQ27220_ADDR,
                                            MESHTASTIC_BQ27220_REG_CURRENT,
                                            &current);

    if(voltage <= 2500U || voltage >= 6000U || soc_rc != 0 || soc > 100U) {
        now_us = monotonic_us();
        if(now_us >= mesh_bq27220_next_fail_log_us) {
            if(current_rc == 0) {
                daemon_event("Telemetry BQ27220 invalid sample voltage=%umV soc=%u%% current=%dmA",
                             voltage, soc, (int)(int16_t)current);
            } else {
                daemon_event("Telemetry BQ27220 invalid sample voltage=%umV soc=%u%% current=NA",
                             voltage, soc);
            }
            mesh_bq27220_next_fail_log_us = now_us + 60000000ULL;
        }
        mesh_bq27220_cache_us = 0ULL;
        return false;
    }

    telemetry->has_device_voltage = true;
    telemetry->device_voltage = (float)voltage / 1000.0f;
    found = true;

    if(soc <= 100U) {
        telemetry->has_battery_level = true;
        telemetry->battery_level = soc;
        found = true;
    }
    if(current_rc == 0) {
        daemon_event("Telemetry BQ27220 voltage=%umV soc=%u%% current=%dmA",
                     voltage, soc, (int)(int16_t)current);
    } else {
        daemon_event("Telemetry BQ27220 voltage=%umV soc=%u%% current=NA",
                     voltage, soc);
    }
    if(found) {
        mesh_bq27220_cache_us = monotonic_us();
        mesh_bq27220_cached_voltage_mv = voltage;
        mesh_bq27220_cached_soc = soc;
        mesh_bq27220_cached_current_ma = (int16_t)current;
    }
    return found;
}

static bool mesh_apply_cached_bq27220_device_metrics(
    mesh_telemetry_info_t *telemetry)
{
    uint64_t age_us;

    if(!telemetry || mesh_bq27220_cache_us == 0ULL) {
        return false;
    }
    age_us = monotonic_us() - mesh_bq27220_cache_us;
    if(age_us > MESHTASTIC_BQ27220_CACHE_TTL_US) {
        return false;
    }
    if(mesh_bq27220_cached_voltage_mv > 2500U &&
       mesh_bq27220_cached_voltage_mv < 6000U) {
        telemetry->has_device_voltage = true;
        telemetry->device_voltage =
            (float)mesh_bq27220_cached_voltage_mv / 1000.0f;
    }
    if(mesh_bq27220_cached_soc <= 100U) {
        telemetry->has_battery_level = true;
        telemetry->battery_level = mesh_bq27220_cached_soc;
    }
    daemon_event("Telemetry BQ27220 cached voltage=%umV soc=%u%% current=%dmA age=%lus",
                 mesh_bq27220_cached_voltage_mv, mesh_bq27220_cached_soc,
                 (int)mesh_bq27220_cached_current_ma,
                 (unsigned long)(age_us / 1000000ULL));
    return telemetry->has_device_voltage || telemetry->has_battery_level;
}

static bool mesh_collect_device_telemetry(mesh_telemetry_info_t *telemetry)
{
    bool bq_ok;
    float channel_util;
    float air_tx;

    if(!telemetry) {
        return false;
    }
    *telemetry = mesh_telemetry_info_t();
    telemetry->has_device_metrics = true;
    telemetry->timestamp = (uint32_t)time(nullptr);
    telemetry->uptime_seconds = (uint32_t)(monotonic_us() / 1000000ULL);
    channel_util = mesh_airtime_channel_util_percent();
    air_tx = mesh_airtime_tx_util_percent();
    telemetry->has_channel_utilization = true;
    telemetry->channel_utilization = channel_util;
    telemetry->has_air_util_tx = true;
    telemetry->air_util_tx = air_tx;
    (void)mesh_read_power_supply_device_metrics(telemetry);
    bq_ok = mesh_read_bq27220_device_metrics(telemetry);
    if(!bq_ok && (!telemetry->has_device_voltage ||
                  !telemetry->has_battery_level)) {
        (void)mesh_apply_cached_bq27220_device_metrics(telemetry);
    }
    return true;
}

static bool mesh_collect_environment_telemetry(mesh_telemetry_info_t *telemetry)
{
    bool ok;

    if(!telemetry) {
        return false;
    }
    *telemetry = mesh_telemetry_info_t();
    telemetry->timestamp = (uint32_t)time(nullptr);
    ok = mesh_read_aht20_hwmon(telemetry) || mesh_read_aht20_i2c(telemetry);
    telemetry->has_environment_metrics = ok;
    return ok;
}

static void nrf9151_gnss_set_state(const char *modem, const char *gps,
                                   const char *detail)
{
    snprintf(mesh_gnss.modem_state, sizeof(mesh_gnss.modem_state), "%s",
             modem && modem[0] ? modem : "-");
    snprintf(mesh_gnss.gps_state, sizeof(mesh_gnss.gps_state), "%s",
             gps && gps[0] ? gps : "-");
    snprintf(mesh_gnss.detail, sizeof(mesh_gnss.detail), "%s",
             detail && detail[0] ? detail : "-");
}

static const char *nrf9151_gnss_phase(void)
{
    if(!mesh_gnss.enabled) {
        return "off";
    }
    if(strcmp(mesh_gnss.modem_state, "missing") == 0 ||
       strcmp(mesh_gnss.modem_state, "off") == 0) {
        return "missing";
    }
    if(strcmp(mesh_gnss.gps_state, "error") == 0 ||
       strcmp(mesh_gnss.gps_state, "failed") == 0) {
        return "error";
    }
    if(mesh_gnss.has_fix) {
        return mesh_gnss.used_cache_fix ? "cache" : "fix";
    }
    if(mesh_gnss.configured && mesh_gnss.nmea_rx_count == 0ULL) {
        return "first";
    }
    if(mesh_gnss.position.sats_in_view == 0U) {
        return "no_sat";
    }
    return "sat_no_fix";
}

static void nrf9151_trim_in_place(char *line)
{
    char *start = line;
    char *end;

    if(!line) {
        return;
    }
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != line) {
        memmove(line, start, strlen(start) + 1U);
    }
    end = line + strlen(line);
    while(end > line && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
}

static int nrf9151_line_has_nmea_prefix(const char *line)
{
    return line && (strncmp(line, "$GP", 3) == 0 ||
                    strncmp(line, "$GN", 3) == 0 ||
                    strncmp(line, "$GA", 3) == 0 ||
                    strncmp(line, "$GB", 3) == 0 ||
                    strncmp(line, "$BD", 3) == 0);
}

static int nrf9151_nmea_checksum_ok(const char *line)
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
    expect = (unsigned int)strtoul(star + 1, nullptr, 16);
    return (calc & 0xffU) == (expect & 0xffU);
}

static int nrf9151_nmea_split(char *body, char **fields, int max_fields)
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

static bool nrf9151_nmea_coord_to_double(const char *value, const char *dir,
                                         double *out)
{
    int deg_width;
    int degrees;
    double raw;
    double decimal;
    char deg_str[4] = { 0 };

    if(!value || !value[0] || !dir || !dir[0] || !out) {
        return false;
    }
    deg_width = (dir[0] == 'N' || dir[0] == 'S') ? 2 : 3;
    if((int)strlen(value) <= deg_width ||
       (size_t)deg_width >= sizeof(deg_str)) {
        return false;
    }
    memcpy(deg_str, value, (size_t)deg_width);
    degrees = atoi(deg_str);
    raw = strtod(value + deg_width, nullptr);
    decimal = (double)degrees + raw / 60.0;
    if(dir[0] == 'S' || dir[0] == 'W') {
        decimal = -decimal;
    }
    if(!isfinite(decimal)) {
        return false;
    }
    *out = decimal;
    return true;
}

static void nrf9151_gnss_write_cache(double lat, double lon, bool has_alt,
                                     double alt_m, uint32_t sats,
                                     const char *source)
{
    if(k230_nrf9151_write_gnss_fix(lat, lon, has_alt ? 1 : 0, alt_m,
                                   (int)sats,
                                   source && source[0] ? source :
                                   "meshtastic") != 0) {
        daemon_event("nRF9151 GNSS cache write failed: %s", strerror(errno));
    }
}

static void nrf9151_gnss_apply_fix(double lat, double lon, bool has_alt,
                                   double alt_m, bool has_speed,
                                   double speed_mps, bool has_track,
                                   double track_deg, uint32_t sats,
                                   bool update_cache = true,
                                   const char *cache_source = "meshtastic")
{
    if(!isfinite(lat) || !isfinite(lon) || lat < -90.0 || lat > 90.0 ||
       lon < -180.0 || lon > 180.0) {
        return;
    }
    mesh_gnss.position.has_latitude = true;
    mesh_gnss.position.has_longitude = true;
    mesh_gnss.position.latitude_i = (int32_t)llround(lat * 10000000.0);
    mesh_gnss.position.longitude_i = (int32_t)llround(lon * 10000000.0);
    mesh_gnss.position.timestamp = (uint32_t)time(nullptr);
    mesh_gnss.position.precision_bits = 32U;
    mesh_gnss.position.sats_in_view = sats;
    if(has_alt && isfinite(alt_m)) {
        mesh_gnss.position.has_altitude = true;
        mesh_gnss.position.altitude_m = (int32_t)llround(alt_m);
    }
    if(has_speed && isfinite(speed_mps) && speed_mps >= 0.0) {
        mesh_gnss.position.has_ground_speed = true;
        mesh_gnss.position.ground_speed_cms = (uint32_t)llround(speed_mps);
    }
    if(has_track && isfinite(track_deg) && track_deg >= 0.0) {
        mesh_gnss.position.has_ground_track = true;
        mesh_gnss.position.ground_track_1e5 =
            (uint32_t)llround(track_deg * 100.0);
    }
    mesh_gnss.has_fix = true;
    mesh_gnss.last_fix_us = monotonic_us();
    mesh_gnss.used_cache_fix = !update_cache;
    if(update_cache) {
        nrf9151_gnss_write_cache(lat, lon, has_alt, alt_m, sats,
                                 cache_source);
    }
    nrf9151_gnss_set_state("present", "fix", "GNSS fix");
    if(!mesh_gnss.first_fix_reported) {
        uint64_t ttff_ms = 0;
        if(mesh_gnss.session_start_us > 0ULL &&
           mesh_gnss.last_fix_us >= mesh_gnss.session_start_us) {
            ttff_ms = (mesh_gnss.last_fix_us - mesh_gnss.session_start_us) /
                      1000ULL;
        }
        mesh_gnss.ttff_ms = ttff_ms;
        mesh_gnss.ttff_valid = true;
        mesh_gnss.first_fix_reported = true;
        daemon_event("nRF9151 GNSS first fix lat=%.7f lon=%.7f sats=%u ttff=%lums",
                     lat, lon, sats, (unsigned long)ttff_ms);
    }
}

static bool nrf9151_gnss_apply_cache_fix(bool quiet)
{
    time_t now = time(nullptr);
    k230_nrf9151_gnss_fix_t fix;

    if(k230_nrf9151_read_gnss_fix(&fix,
                                  MESHTASTIC_NRF9151_FIX_CACHE_MAX_AGE_SEC) != 0) {
        return false;
    }
    if(mesh_gnss.cache_epoch == fix.epoch && mesh_gnss.has_fix &&
       strcmp(mesh_gnss.gps_state, "fix") == 0) {
        return true;
    }

    nrf9151_gnss_apply_fix(fix.latitude, fix.longitude,
                           fix.has_altitude != 0, fix.altitude_m, false,
                           0.0, false, 0.0,
                           fix.satellites > 0 ? (uint32_t)fix.satellites : 0U,
                           false,
                           "lte-cache");
    if(fix.epoch > 0) {
        mesh_gnss.position.timestamp = (uint32_t)fix.epoch;
    }
    mesh_gnss.cache_epoch = fix.epoch;
    mesh_gnss.used_cache_fix = true;
    mesh_gnss.ttff_valid = false;
    nrf9151_gnss_set_state("present", "fix", "GNSS fix from LTE cache");
    if(!quiet) {
        long age = (now > 0 && fix.epoch > 0 && now >= fix.epoch) ?
                   (long)(now - fix.epoch) : fix.age_seconds;
        daemon_event("nRF9151 GNSS cache applied source=%s lat=%.7f lon=%.7f age=%lds",
                     fix.source, fix.latitude, fix.longitude, age);
    }
    return true;
}

static void nrf9151_gnss_parse_gga(char **fields, int count)
{
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    int fix = count > 6 ? atoi(fields[6]) : 0;
    int sats = count > 7 ? atoi(fields[7]) : 0;

    if(sats > 0) {
        mesh_gnss.position.sats_in_view = (uint32_t)sats;
    }
    if(fix <= 0 || count <= 9 ||
       !nrf9151_nmea_coord_to_double(fields[2], fields[3], &lat) ||
       !nrf9151_nmea_coord_to_double(fields[4], fields[5], &lon)) {
        mesh_gnss.nmea_nofix_count++;
        if(nrf9151_gnss_apply_cache_fix(true)) {
            return;
        }
        nrf9151_gnss_set_state("present", "searching",
                               sats > 0 ? "GNSS satellites visible no fix" :
                               "GNSS running no satellites");
        return;
    }
    alt = fields[9] && fields[9][0] ? strtod(fields[9], nullptr) : 0.0;
    nrf9151_gnss_apply_fix(lat, lon, fields[9] && fields[9][0], alt, false,
                           0.0, false, 0.0, sats > 0 ? (uint32_t)sats : 0U);
}

static void nrf9151_gnss_parse_rmc(char **fields, int count)
{
    double lat = 0.0;
    double lon = 0.0;
    double speed_mps = 0.0;
    double track = 0.0;

    if(count <= 8 || fields[2][0] != 'A' ||
       !nrf9151_nmea_coord_to_double(fields[3], fields[4], &lat) ||
       !nrf9151_nmea_coord_to_double(fields[5], fields[6], &lon)) {
        return;
    }
    if(fields[7] && fields[7][0]) {
        speed_mps = strtod(fields[7], nullptr) * 0.514444;
    }
    if(fields[8] && fields[8][0]) {
        track = strtod(fields[8], nullptr);
    }
    nrf9151_gnss_apply_fix(lat, lon, false, 0.0,
                           fields[7] && fields[7][0], speed_mps,
                           fields[8] && fields[8][0], track,
                           mesh_gnss.position.sats_in_view);
}

static void nrf9151_gnss_parse_gsv(char **fields, int count)
{
    int sats;

    if(count <= 3) {
        return;
    }
    sats = atoi(fields[3]);
    if(sats >= 0) {
        mesh_gnss.position.sats_in_view = (uint32_t)sats;
        if(!mesh_gnss.has_fix) {
            nrf9151_gnss_set_state("present", "searching",
                                   sats > 0 ? "GNSS satellites visible no fix" :
                                   "GNSS running no satellites");
        }
    }
}

static void nrf9151_gnss_parse_sentence(const char *line)
{
    char body[MESHTASTIC_NRF9151_LINE_MAX];
    char *fields[32];
    const char *star;
    size_t len;
    int count;
    const char *type;

    if(!line || line[0] != '$' || !nrf9151_nmea_checksum_ok(line)) {
        return;
    }
    mesh_gnss.nmea_valid_count++;
    star = strchr(line, '*');
    len = star ? (size_t)(star - line - 1) : strlen(line + 1);
    if(len >= sizeof(body)) {
        len = sizeof(body) - 1U;
    }
    memcpy(body, line + 1, len);
    body[len] = '\0';
    count = nrf9151_nmea_split(body, fields,
                               (int)(sizeof(fields) / sizeof(fields[0])));
    if(count <= 0 || strlen(fields[0]) < 5U) {
        return;
    }
    type = fields[0] + strlen(fields[0]) - 3U;
    if(strcmp(type, "GGA") == 0) {
        nrf9151_gnss_parse_gga(fields, count);
    } else if(strcmp(type, "RMC") == 0) {
        nrf9151_gnss_parse_rmc(fields, count);
    } else if(strcmp(type, "GSV") == 0) {
        nrf9151_gnss_parse_gsv(fields, count);
    }
}

static void nrf9151_gnss_parse_pos_urc(const char *line)
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

    if(!line) {
        return;
    }
    p = strchr(line, ':');
    if(!p) {
        return;
    }
    p++;
    while(*p && isspace((unsigned char)*p)) {
        p++;
    }
    parsed = sscanf(p, "%lf,%lf,%lf,%lf,%lf,%lf,\"%63[^\"]\"",
                    &lat, &lon, &alt, &acc, &speed, &heading, datetime);
    if(parsed < 2) {
        return;
    }
    (void)acc;
    (void)datetime;
    nrf9151_gnss_apply_fix(lat, lon, parsed >= 3, alt, parsed >= 5,
                           speed, parsed >= 6, heading,
                           mesh_gnss.position.sats_in_view);
}

static void nrf9151_gnss_process_line(char *line)
{
    const char *nmea = nullptr;

    if(!line) {
        return;
    }
    nrf9151_trim_in_place(line);
    if(!line[0]) {
        return;
    }
    mesh_gnss.last_rx_us = monotonic_us();
    if(strncmp(line, "#XGNSSNMEA:", 11) == 0) {
        nmea = line + 11;
        while(*nmea && isspace((unsigned char)*nmea)) {
            nmea++;
        }
    } else if(nrf9151_line_has_nmea_prefix(line)) {
        nmea = line;
    }
    if(nmea && nrf9151_line_has_nmea_prefix(nmea)) {
        mesh_gnss.nmea_rx_count++;
        mesh_gnss.last_nmea_us = mesh_gnss.last_rx_us;
        nrf9151_gnss_parse_sentence(nmea);
        return;
    }
    if(strncmp(line, "#XGNSSPOS:", 10) == 0) {
        nrf9151_gnss_parse_pos_urc(line);
        return;
    }
    if(strncmp(line, "#XGNSS:", 7) == 0 ||
       strncmp(line, "#XNMEA:", 7) == 0) {
        nrf9151_gnss_set_state("present", mesh_gnss.has_fix ? "fix" :
                               "searching", line);
    }
}

static void nrf9151_gnss_feed_bytes(const char *buf, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        char c = buf[i];

        if(c == '\r') {
            continue;
        }
        if(c == '\n') {
            mesh_gnss.line[mesh_gnss.line_used] = '\0';
            nrf9151_gnss_process_line(mesh_gnss.line);
            mesh_gnss.line_used = 0;
            continue;
        }
        if(mesh_gnss.line_used + 1U >= sizeof(mesh_gnss.line)) {
            mesh_gnss.line[mesh_gnss.line_used] = '\0';
            nrf9151_gnss_process_line(mesh_gnss.line);
            mesh_gnss.line_used = 0;
        }
        mesh_gnss.line[mesh_gnss.line_used++] = c;
    }
}

static void nrf9151_gnss_process_response(const char *resp)
{
    char line[MESHTASTIC_NRF9151_LINE_MAX];
    size_t used = 0;

    if(!resp) {
        return;
    }
    for(const char *p = resp; *p; p++) {
        char c = *p;
        if(c == '\r') {
            continue;
        }
        if(c == '\n') {
            line[used] = '\0';
            nrf9151_gnss_process_line(line);
            used = 0;
            continue;
        }
        if(used + 1U >= sizeof(line)) {
            line[used] = '\0';
            nrf9151_gnss_process_line(line);
            used = 0;
        }
        line[used++] = c;
    }
    if(used > 0U) {
        line[used] = '\0';
        nrf9151_gnss_process_line(line);
    }
}

static int nrf9151_configure_uart3_iomux(void)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        return -1;
    }
    map = mmap(NULL, K230_MESH_IOMUX_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, K230_MESH_IOMUX_BASE);
    if(map == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    regs = (volatile uint32_t *)map;
    regs[K230_MESH_IOMUX_IO28_OFFSET / 4U] =
        K230_MESH_IOMUX_FUNC_ALT2 | K230_MESH_IOMUX_IE_BIT |
        K230_MESH_IOMUX_OE_BIT | K230_MESH_IOMUX_ST_BIT |
        K230_MESH_IOMUX_DS_8MA;
    regs[K230_MESH_IOMUX_IO29_OFFSET / 4U] =
        K230_MESH_IOMUX_FUNC_ALT2 | K230_MESH_IOMUX_IE_BIT |
        K230_MESH_IOMUX_ST_BIT | K230_MESH_IOMUX_DS_8MA;
    regs[K230_MESH_IOMUX_IO50_OFFSET / 4U] =
        K230_MESH_IOMUX_FUNC_ALT0 | K230_MESH_IOMUX_DS_8MA;
    regs[K230_MESH_IOMUX_IO51_OFFSET / 4U] =
        K230_MESH_IOMUX_FUNC_ALT0 | K230_MESH_IOMUX_DS_8MA;
    munmap(map, K230_MESH_IOMUX_SIZE);
    close(fd);
    return 0;
}

static int nrf9151_open_uart(const std::string &path)
{
    (void)path;
    errno = ENOTSUP;
    return -1;
}

static bool nrf9151_response_has_token(const char *resp, const char *token)
{
    const char *p = resp;
    size_t token_len;

    if(!resp || !token) {
        return false;
    }
    token_len = strlen(token);
    while(*p) {
        while(*p == '\r' || *p == '\n' || isspace((unsigned char)*p)) {
            p++;
        }
        if(strncmp(p, token, token_len) == 0 &&
           (p[token_len] == '\0' || p[token_len] == '\r' ||
            p[token_len] == '\n' || isspace((unsigned char)p[token_len]))) {
            return true;
        }
        while(*p && *p != '\r' && *p != '\n') {
            p++;
        }
    }
    return false;
}

static int nrf9151_exchange(int fd, const char *cmd, char *resp,
                            size_t resp_len, uint64_t timeout_us)
{
    std::string wire;
    uint64_t start;
    size_t used = 0;

    if(resp && resp_len > 0U) {
        resp[0] = '\0';
    }
    if(fd < 0 || !cmd) {
        return -1;
    }
    wire = std::string(cmd) + "\r\n";
    if(write(fd, wire.c_str(), wire.size()) < 0) {
        return -1;
    }
    start = monotonic_us();
    while(monotonic_us() - start < timeout_us) {
        struct pollfd pfd;
        char buf[128];
        int rc;
        ssize_t n;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = POLLIN;
        rc = poll(&pfd, 1, 80);
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(rc == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }
        n = read(fd, buf, sizeof(buf));
        if(n < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(n == 0) {
            continue;
        }
        if(resp && resp_len > 0U && used + 1U < resp_len) {
            size_t copy = (size_t)n;
            if(copy > resp_len - used - 1U) {
                copy = resp_len - used - 1U;
            }
            memcpy(resp + used, buf, copy);
            used += copy;
            resp[used] = '\0';
        }
        nrf9151_gnss_feed_bytes(buf, (size_t)n);
        if(resp && (nrf9151_response_has_token(resp, "OK") ||
                    nrf9151_response_has_token(resp, "ERROR"))) {
            return nrf9151_response_has_token(resp, "OK") ? 0 : 1;
        }
    }
    return -1;
}

static void nrf9151_gnss_close_uart(void)
{
    if(mesh_gnss.fd >= 0) {
        close(mesh_gnss.fd);
        mesh_gnss.fd = -1;
    }
    if(mesh_gnss.lock_fd >= 0) {
        k230_nrf9151_release_uart(mesh_gnss.lock_fd);
        mesh_gnss.lock_fd = -1;
    }
}

static void nrf9151_gnss_read_available(void)
{
    for(;;) {
        char buf[256];
        ssize_t n;

        if(mesh_gnss.fd < 0) {
            return;
        }
        n = read(mesh_gnss.fd, buf, sizeof(buf));
        if(n < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            daemon_event("nRF9151 GNSS read failed: %s", strerror(errno));
            nrf9151_gnss_close_uart();
            mesh_gnss.present = false;
            mesh_gnss.configured = false;
            nrf9151_gnss_set_state("error", "unavailable", "UART read error");
            return;
        }
        if(n == 0) {
            return;
        }
        nrf9151_gnss_feed_bytes(buf, (size_t)n);
    }
}

static int nrf9151_response_gnss_active(const char *resp)
{
    const char *p;

    if(!resp) {
        return -1;
    }
    p = strstr(resp, "#XGNSS:");
    if(!p) {
        p = strstr(resp, "GNSS:");
    }
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

static int nrf9151_response_cfun_mode(const char *resp)
{
    const char *p;

    if(!resp) {
        return -1;
    }
    p = strstr(resp, "+CFUN:");
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

static int nrf9151_response_systemmode(const char *resp, int *lte_m,
                                       int *nb_iot, int *gnss,
                                       int *preference)
{
    const char *p;
    int a;
    int b;
    int c;
    int d;

    if(lte_m) {
        *lte_m = -1;
    }
    if(nb_iot) {
        *nb_iot = -1;
    }
    if(gnss) {
        *gnss = -1;
    }
    if(preference) {
        *preference = -1;
    }
    if(!resp) {
        return -1;
    }
    p = strstr(resp, "%XSYSTEMMODE:");
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
    if(sscanf(p, "%d,%d,%d,%d", &a, &b, &c, &d) != 4) {
        return -1;
    }
    if(lte_m) {
        *lte_m = a;
    }
    if(nb_iot) {
        *nb_iot = b;
    }
    if(gnss) {
        *gnss = c;
    }
    if(preference) {
        *preference = d;
    }
    return 0;
}

static bool nrf9151_gnss_prepare_lte_mode_locked(void)
{
    char resp[MESHTASTIC_NRF9151_RESPONSE_MAX];
    int rc;
    int cfun = -1;
    int lte_m = -1;
    int nb_iot = -1;
    int gnss = -1;
    int pref = -1;
    int failures = 0;

    rc = nrf9151_exchange(mesh_gnss.fd, "AT%XSYSTEMMODE?", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0 ||
       nrf9151_response_systemmode(resp, &lte_m, &nb_iot, &gnss,
                                   &pref) != 0) {
        daemon_event("nRF9151 GNSS systemmode query failed rc=%d", rc);
        return false;
    }
    if(gnss != 1) {
        daemon_event("nRF9151 GNSS not enabled in systemmode lte_m=%d nb=%d gnss=%d pref=%d; skip CFUN=0 auto-reconfigure",
                     lte_m, nb_iot, gnss, pref);
        return false;
    }

    rc = nrf9151_exchange(mesh_gnss.fd, "AT+CFUN?", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc == 0) {
        cfun = nrf9151_response_cfun_mode(resp);
    }
    if(cfun == 0 || cfun == 4 || cfun < 0) {
        rc = nrf9151_exchange(mesh_gnss.fd, "AT+CFUN=1", resp, sizeof(resp),
                              MESHTASTIC_NRF9151_CMD_TIMEOUT_US * 3ULL);
    } else {
        rc = nrf9151_exchange(mesh_gnss.fd, "AT+CFUN=31", resp, sizeof(resp),
                              MESHTASTIC_NRF9151_CMD_TIMEOUT_US * 3ULL);
    }
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        failures++;
    }
    daemon_event("nRF9151 GNSS preserve LTE mode lte_m=%d nb=%d gnss=%d pref=%d cfun=%d rc=%d",
                 lte_m, nb_iot, gnss, pref, cfun, rc);
    return failures == 0;
}

static bool nrf9151_gnss_restart_session_locked(const probe_options_t &opts,
                                                const char *reason)
{
    char resp[MESHTASTIC_NRF9151_RESPONSE_MAX];
    int rc;
    int failures = 0;

    if(mesh_gnss.fd < 0) {
        return false;
    }

    mesh_gnss.last_search_restart_us = monotonic_us();
    mesh_gnss.session_start_us = mesh_gnss.last_search_restart_us;
    mesh_gnss.first_fix_reported = false;
    mesh_gnss.line_used = 0;
    daemon_event("nRF9151 GNSS restart reason=%s uart=%s",
                 reason && reason[0] ? reason : "search-timeout",
                 opts.gps_uart_path.c_str());

    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XGNSS=0", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        failures++;
    }
    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XNMEA=0", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        failures++;
    }
    usleep(120000);

    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XNMEA=1", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        failures++;
    }
    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XGNSS=1,0,0,0", resp,
                          sizeof(resp), MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        char status[MESHTASTIC_NRF9151_RESPONSE_MAX];
        int status_rc = nrf9151_exchange(mesh_gnss.fd, "AT#XGNSS?", status,
                                         sizeof(status),
                                         MESHTASTIC_NRF9151_CMD_TIMEOUT_US);

        nrf9151_gnss_process_response(status);
        if(status_rc != 0 || nrf9151_response_gnss_active(status) <= 0) {
            failures++;
        }
    }

    mesh_gnss.configured = failures < 3;
    nrf9151_gnss_set_state("present",
                           mesh_gnss.has_fix ? "fix" : "searching",
                           failures < 3 ? "GNSS restart waiting" :
                                          "GNSS restart failed");
    return failures < 3;
}

static bool nrf9151_gnss_start_locked(const probe_options_t &opts)
{
    char resp[MESHTASTIC_NRF9151_RESPONSE_MAX];
    int rc;
    int failures = 0;

    if(mesh_gnss.configured) {
        return true;
    }
    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XGNSS?", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc == 0 && nrf9151_response_gnss_active(resp) > 0) {
        rc = nrf9151_exchange(mesh_gnss.fd, "AT#XNMEA=1", resp, sizeof(resp),
                              MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
        nrf9151_gnss_process_response(resp);
        mesh_gnss.configured = rc == 0;
        if(mesh_gnss.configured) {
            if(mesh_gnss.session_start_us == 0ULL) {
                mesh_gnss.session_start_us = monotonic_us();
                mesh_gnss.first_fix_reported = false;
            }
            nrf9151_gnss_set_state("present", "searching",
                                   "GNSS already running");
            daemon_event("nRF9151 GNSS already running uart=%s",
                         opts.gps_uart_path.c_str());
        }
        return mesh_gnss.configured;
    }

    if(!nrf9151_gnss_prepare_lte_mode_locked()) {
        failures++;
    }
    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XNMEA=1", resp, sizeof(resp),
                          MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        failures++;
        daemon_event("nRF9151 GNSS NMEA enable failed rc=%d", rc);
    }
    mesh_gnss.session_start_us = monotonic_us();
    mesh_gnss.first_fix_reported = false;
    rc = nrf9151_exchange(mesh_gnss.fd, "AT#XGNSS=1,0,0,0", resp,
                          sizeof(resp), MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
    nrf9151_gnss_process_response(resp);
    if(rc != 0) {
        char status[MESHTASTIC_NRF9151_RESPONSE_MAX];
        int status_rc = nrf9151_exchange(mesh_gnss.fd, "AT#XGNSS?", status,
                                         sizeof(status),
                                         MESHTASTIC_NRF9151_CMD_TIMEOUT_US);
        nrf9151_gnss_process_response(status);
        if(status_rc != 0 || nrf9151_response_gnss_active(status) <= 0) {
            nrf9151_gnss_set_state("present", "error", "GNSS start failed");
            daemon_event("nRF9151 GNSS start failed rc=%d status_rc=%d",
                         rc, status_rc);
            return false;
        }
    }
    mesh_gnss.configured = true;
    nrf9151_gnss_set_state("present", "searching", "GNSS running");
    daemon_event("nRF9151 GNSS started uart=%s interval=%us setup_failures=%d",
                 opts.gps_uart_path.c_str(), opts.position_interval_sec,
                 failures);
    return true;
}

static void nrf9151_gnss_poll(const probe_options_t &opts, uint64_t now)
{
    k230_nrf9151_status_t status;

    if(!opts.position_enabled) {
        nrf9151_gnss_close_uart();
        mesh_gnss.enabled = false;
        nrf9151_gnss_set_state("off", "off", "Position disabled");
        return;
    }
    mesh_gnss.enabled = true;
    mesh_gnss.probed = true;
    nrf9151_gnss_close_uart();

    if(!k230_nrf9151_uart_present()) {
        mesh_gnss.present = false;
        mesh_gnss.configured = false;
        nrf9151_gnss_set_state("missing", "unavailable",
                               "nRF9151 UART missing");
        return;
    }

    if(k230_nrf9151_read_status(&status, 0) != 0) {
        mesh_gnss.present = true;
        mesh_gnss.configured = false;
        if(nrf9151_gnss_apply_cache_fix(true)) {
            return;
        }
        nrf9151_gnss_set_state("present", "starting",
                               "Waiting nRF9151 manager status");
        return;
    }

    mesh_gnss.present = status.present || k230_nrf9151_uart_present();
    mesh_gnss.configured = status.gnss_running;
    if(!status.gnss_running) {
        if(now >= mesh_gnss.next_probe_us) {
            mesh_gnss.next_probe_us = now + MESHTASTIC_POSITION_RETRY_US;
            daemon_event("nRF9151 GNSS cache is not running; waiting launcher manager");
        }
        if(nrf9151_gnss_apply_cache_fix(true)) {
            return;
        }
        nrf9151_gnss_set_state("present", "starting",
                               "Waiting launcher nRF9151 manager");
        return;
    }
    mesh_gnss.nmea_rx_count = status.nmea_rx_count;
    mesh_gnss.nmea_nofix_count = status.nmea_nofix_count;
    mesh_gnss.position.sats_in_view = status.satellites;
    if(status.nmea_rx_count > 0U) {
        mesh_gnss.last_nmea_us = now;
        mesh_gnss.last_rx_us = now;
    }
    if(status.ttff_ms > 0UL) {
        mesh_gnss.ttff_ms = status.ttff_ms;
        mesh_gnss.ttff_valid = true;
        mesh_gnss.first_fix_reported = true;
    }
    if(mesh_gnss.session_start_us == 0ULL && status.gnss_running) {
        mesh_gnss.session_start_us = now;
    }

    if(status.gnss_has_fix) {
        nrf9151_gnss_apply_fix(status.latitude, status.longitude,
                               status.has_altitude != 0,
                               status.altitude_m, false, 0.0,
                               false, 0.0, status.satellites,
                               true, "nrf9151-manager");
        mesh_gnss.used_cache_fix = false;
        return;
    }

    if(nrf9151_gnss_apply_cache_fix(true)) {
        return;
    }

    mesh_gnss.has_fix = false;
    nrf9151_gnss_set_state(status.present ? "present" : "missing",
                           status.gnss_phase[0] ? status.gnss_phase :
                           (status.gnss_running ? "searching" :
                            "unavailable"),
                           status.gnss_status[0] ? status.gnss_status :
                           "Waiting nRF9151 GNSS");
}

static void nrf9151_gnss_close(void)
{
    nrf9151_gnss_close_uart();
    mesh_gnss.configured = false;
}

static bool fixed_position_from_opts(const probe_options_t &opts,
                                     mesh_position_info_t *position)
{
    if(!position || !opts.fixed_position_enabled ||
       opts.fixed_position_latitude_i == 0 ||
       opts.fixed_position_longitude_i == 0) {
        return false;
    }
    *position = mesh_position_info_t();
    position->has_latitude = true;
    position->has_longitude = true;
    position->latitude_i = opts.fixed_position_latitude_i;
    position->longitude_i = opts.fixed_position_longitude_i;
    position->has_altitude = opts.fixed_position_has_altitude;
    position->altitude_m = opts.fixed_position_altitude_m;
    position->timestamp = mesh_now_epoch();
    return true;
}

static bool encode_position_proto(const mesh_position_info_t &position,
                                  uint32_t next_update_sec,
                                  std::vector<uint8_t> *out)
{
    if(!out || !position.has_latitude || !position.has_longitude) {
        return false;
    }
    out->clear();
    append_sfixed32_field(out, 1U, position.latitude_i);
    append_sfixed32_field(out, 2U, position.longitude_i);
    if(position.has_altitude) {
        append_uint32_field(out, 3U, (uint32_t)position.altitude_m);
    }
    if(position.timestamp != 0U) {
        append_varint(out, (7U << 3U) | 5U);
        append_fixed32(out, position.timestamp);
    }
    append_uint32_field(out, 5U, 2U);
    if(position.has_altitude) {
        append_uint32_field(out, 6U, 2U);
    }
    if(position.has_ground_speed) {
        append_uint32_field(out, 15U, position.ground_speed_cms);
    }
    if(position.has_ground_track) {
        append_uint32_field(out, 16U, position.ground_track_1e5);
    }
    if(position.sats_in_view != 0U) {
        append_uint32_field(out, 19U, position.sats_in_view);
    }
    if(next_update_sec != 0U) {
        append_uint32_field(out, 21U, next_update_sec);
    }
    if(position.precision_bits != 0U) {
        append_uint32_field(out, 23U, position.precision_bits);
    }
    return !out->empty();
}

static bool encode_waypoint_proto(const mesh_waypoint_info_t &waypoint,
                                  std::vector<uint8_t> *out)
{
    if(!out || !waypoint.has_latitude || !waypoint.has_longitude) {
        return false;
    }
    out->clear();
    if(waypoint.has_id) {
        append_uint32_field(out, 1U, waypoint.id);
    }
    append_sfixed32_field(out, 2U, waypoint.latitude_i);
    append_sfixed32_field(out, 3U, waypoint.longitude_i);
    if(waypoint.expire != 0U) {
        append_uint32_field(out, 4U, waypoint.expire);
    }
    if(waypoint.locked_to != 0U) {
        append_uint32_field(out, 5U, waypoint.locked_to);
    }
    append_string_field(out, 6U, waypoint.name, 31U);
    append_string_field(out, 7U, waypoint.description, 95U);
    if(waypoint.icon != 0U) {
        append_fixed32_field(out, 8U, waypoint.icon);
    }
    return !out->empty() && out->size() <= MESHTASTIC_DATA_PAYLOAD_LEN;
}

static bool encode_device_metrics_proto(const mesh_telemetry_info_t &telemetry,
                                        std::vector<uint8_t> *out)
{
    if(!out || !telemetry.has_device_metrics) {
        return false;
    }
    out->clear();
    if(telemetry.has_battery_level) {
        append_uint32_field(out, 1U, telemetry.battery_level);
    }
    if(telemetry.has_device_voltage) {
        append_float_field(out, 2U, telemetry.device_voltage);
    }
    if(telemetry.has_channel_utilization) {
        append_float_field(out, 3U, telemetry.channel_utilization);
    }
    if(telemetry.has_air_util_tx) {
        append_float_field(out, 4U, telemetry.air_util_tx);
    }
    if(telemetry.uptime_seconds != 0U) {
        append_uint32_field(out, 5U, telemetry.uptime_seconds);
    }
    return !out->empty();
}

static bool encode_environment_metrics_proto(
    const mesh_telemetry_info_t &telemetry, std::vector<uint8_t> *out)
{
    if(!out || !telemetry.has_environment_metrics) {
        return false;
    }
    out->clear();
    if(telemetry.has_temperature) {
        append_float_field(out, 1U, telemetry.temperature_c);
    }
    if(telemetry.has_humidity) {
        append_float_field(out, 2U, telemetry.humidity_percent);
    }
    if(telemetry.has_pressure) {
        append_float_field(out, 3U, telemetry.pressure_hpa);
    }
    if(telemetry.has_environment_voltage) {
        append_float_field(out, 5U, telemetry.environment_voltage);
    }
    if(telemetry.has_iaq) {
        append_uint32_field(out, 7U, telemetry.iaq);
    }
    return !out->empty();
}

static bool encode_telemetry_proto(const mesh_telemetry_info_t &telemetry,
                                   bool environment,
                                   std::vector<uint8_t> *out)
{
    std::vector<uint8_t> metrics;

    if(!out) {
        return false;
    }
    out->clear();
    if(environment) {
        if(!encode_environment_metrics_proto(telemetry, &metrics)) {
            return false;
        }
    } else if(!encode_device_metrics_proto(telemetry, &metrics)) {
        return false;
    }
    if(telemetry.timestamp != 0U) {
        append_varint(out, (1U << 3U) | 5U);
        append_fixed32(out, telemetry.timestamp);
    }
    append_bytes_field(out, environment ? 3U : 2U, metrics);
    return !out->empty();
}

static bool encode_telemetry_request_proto(bool environment,
                                           std::vector<uint8_t> *out)
{
    if(!out) {
        return false;
    }
    out->clear();
    append_bytes_field(out, environment ? 3U : 2U, std::vector<uint8_t>());
    return true;
}

static bool encode_route_discovery_response_proto(const probe_options_t &opts,
                                                  const mesh_header_t &
                                                      rx_header,
                                                  float rx_snr,
                                                  std::vector<uint8_t> *out)
{
    int32_t snr_q4;

    (void)opts;
    (void)rx_header;
    if(!out) {
        return false;
    }
    out->clear();
    if(!isfinite(rx_snr)) {
        rx_snr = 0.0f;
    }
    snr_q4 = (int32_t)roundf(rx_snr * 4.0f);
    if(snr_q4 < -128) {
        snr_q4 = -128;
    } else if(snr_q4 > 127) {
        snr_q4 = 127;
    }
    append_sint32_field(out, 4U, snr_q4);
    return !out->empty();
}

static bool encode_neighbor_info_proto(const probe_options_t &opts,
                                       std::vector<uint8_t> *out)
{
    uint32_t now = (uint32_t)time(nullptr);
    size_t added = 0;

    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(out, 1U, opts.from_node);
    append_uint32_field(out, 2U, opts.from_node);
    append_uint32_field(out, 3U, opts.nodeinfo_interval_sec);
    for(size_t i = 0; i < mesh_node_count && added < 8U; i++) {
        std::vector<uint8_t> entry;

        if(mesh_nodes[i].node == 0U || mesh_nodes[i].node == opts.from_node) {
            continue;
        }
        append_uint32_field(&entry, 1U, mesh_nodes[i].node);
        append_float_field(&entry, 2U, mesh_nodes[i].snr);
        append_varint(&entry, (3U << 3U) | 5U);
        append_fixed32(&entry, now);
        append_uint32_field(&entry, 4U, opts.nodeinfo_interval_sec);
        append_bytes_field(out, 4U, entry);
        added++;
    }
    (void)added;
    return !out->empty();
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
    if(mesh_pki_public_key_available()) {
        append_bytes_field(out, 8U, mesh_pki_identity.public_key,
                           MESHTASTIC_CURVE25519_KEY_LEN);
    }
    return true;
}

static uint32_t phoneapi_nodedb_count(uint32_t local_node)
{
    uint32_t count = 1U;

    for(size_t i = 0; i < mesh_node_count; i++) {
        if(mesh_nodes[i].node != 0U && mesh_nodes[i].node != local_node) {
            count++;
        }
    }
    return count;
}

static bool encode_phoneapi_my_node_info(const probe_options_t &opts,
                                         uint32_t nodedb_count,
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
    append_uint32_field(out, 15U, nodedb_count == 0U ? 1U : nodedb_count);
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

static bool phoneapi_get_interface_ipv4(const char *ifname, uint32_t *ip_le)
{
    int fd;
    struct ifreq ifr;
    struct sockaddr_in *addr;

    if(ip_le) {
        *ip_le = 0U;
    }
    if(!ifname || !ifname[0]) {
        return false;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        return false;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if(ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        close(fd);
        return false;
    }
    close(fd);
    addr = (struct sockaddr_in *)&ifr.ifr_addr;
    if(ip_le) {
        *ip_le = ntohl(addr->sin_addr.s_addr);
    }
    return addr->sin_addr.s_addr != 0U;
}

static bool encode_phoneapi_network_connection_status(const char *ifname,
                                                      std::vector<uint8_t> *out)
{
    uint32_t ip = 0U;
    bool connected = phoneapi_get_interface_ipv4(ifname, &ip);

    if(!out) {
        return false;
    }
    out->clear();
    if(ip != 0U) {
        append_fixed32_field(out, 1U, ip);
    }
    append_bool_field(out, 2U, connected);
    append_bool_field(out, 3U, false);
    append_bool_field(out, 4U, false);
    return true;
}

static bool encode_phoneapi_connection_status(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> network;
    std::vector<uint8_t> entry;
    char pair_code[16];
    phoneapi_bridge_state_t ble_state;
    bool ble_connected;

    if(!out) {
        return false;
    }
    out->clear();

    if(encode_phoneapi_network_connection_status("wlan0", &network)) {
        entry.clear();
        append_bytes_field(&entry, 1U, network);
        append_bytes_field(out, 1U, entry);
    }
    if(encode_phoneapi_network_connection_status("eth0", &network)) {
        entry.clear();
        append_bytes_field(&entry, 1U, network);
        append_bytes_field(out, 2U, entry);
    }

    ble_state = phoneapi_bridge_get_state(nullptr, 0);
    ble_connected = ble_state == PHONEAPI_BRIDGE_CONNECTED;
    entry.clear();
    if(phoneapi_bridge_get_pairing_code(pair_code, sizeof(pair_code))) {
        append_uint32_field(&entry, 1U, (uint32_t)strtoul(pair_code, nullptr,
                                                          10));
    }
    append_bool_field(&entry, 3U, ble_connected);
    append_bytes_field(out, 3U, entry);

    entry.clear();
    append_uint32_field(&entry, 1U, 115200U);
    append_bool_field(&entry, 2U, phoneapi_bridge_can_send());
    append_bytes_field(out, 4U, entry);
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
        REGION_UA_868 = 15U,
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
        REGION_UA_433,  REGION_UA_868, REGION_MY_433, REGION_MY_919,
        REGION_SG_923,
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

static bool encode_phoneapi_config_position(const probe_options_t &opts,
                                            std::vector<uint8_t> *out)
{
    std::vector<uint8_t> position;
    uint32_t flags = 0x0001U | 0x0020U | 0x0080U | 0x0100U | 0x0200U;
    uint32_t gps_mode = opts.position_enabled ? 1U : 0U;

    if(!out) {
        return false;
    }
    out->clear();
    if(opts.position_enabled && mesh_gnss.probed && !mesh_gnss.present) {
        gps_mode = 2U;
    }
    append_uint32_field(&position, 1U, opts.position_interval_sec);
    append_bool_field(&position, 2U, false);
    append_bool_field(&position, 3U, opts.fixed_position_enabled);
    append_uint32_field(&position, 5U, 30U);
    append_uint32_field(&position, 7U, flags);
    append_uint32_field(&position, 8U, 29U);
    append_uint32_field(&position, 9U, 28U);
    append_uint32_field(&position, 12U, 2U);
    append_uint32_field(&position, 13U, gps_mode);
    append_bytes_field(out, 2U, position);
    return true;
}

static bool encode_phoneapi_config_power(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> power;

    if(!out) {
        return false;
    }
    out->clear();
    append_bool_field(&power, 1U, false);
    append_uint32_field(&power, 4U, 60U);
    append_uint32_field(&power, 7U, 300U);
    append_uint32_field(&power, 8U, 10U);
    append_bytes_field(out, 3U, power);
    return true;
}

static bool encode_phoneapi_config_network(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> network;

    if(!out) {
        return false;
    }
    out->clear();
    append_bool_field(&network, 1U, false);
    append_string_field(&network, 5U, "pool.ntp.org", 32U);
    append_bool_field(&network, 6U, true);
    append_uint32_field(&network, 7U, 0U);
    append_bool_field(&network, 11U, false);
    append_bytes_field(out, 4U, network);
    return true;
}

static bool encode_phoneapi_config_display(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> display;

    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(&display, 1U, 60U);
    append_bool_field(&display, 5U, false);
    append_uint32_field(&display, 6U, 0U);
    append_uint32_field(&display, 7U, 0U);
    append_uint32_field(&display, 8U, 3U);
    append_bool_field(&display, 9U, true);
    append_bool_field(&display, 10U, true);
    append_bytes_field(out, 5U, display);
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

static bool encode_phoneapi_config_security(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> security;

    if(!out) {
        return false;
    }
    out->clear();
    append_bool_field(&security, 4U, false);
    append_bool_field(&security, 5U, false);
    append_bool_field(&security, 6U, false);
    append_bool_field(&security, 8U, true);
    append_uint32_field(&security, 9U, 0U);
    append_bytes_field(out, 8U, security);
    return true;
}

static bool encode_phoneapi_config_sessionkey(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> sessionkey;

    if(!out) {
        return false;
    }
    out->clear();
    append_bytes_field(out, 9U, sessionkey);
    return true;
}

static bool encode_phoneapi_config_device_ui(std::vector<uint8_t> *out)
{
    std::vector<uint8_t> device_ui;

    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(&device_ui, 1U, 1U);
    append_uint32_field(&device_ui, 2U, 180U);
    append_uint32_field(&device_ui, 3U, 60U);
    append_bool_field(&device_ui, 4U, false);
    append_bool_field(&device_ui, 5U, false);
    append_bool_field(&device_ui, 8U, true);
    append_bool_field(&device_ui, 9U, true);
    append_uint32_field(&device_ui, 17U, 0x25C281U);
    append_bytes_field(out, 10U, device_ui);
    return true;
}

static bool encode_phoneapi_module_config_telemetry(
    const probe_options_t &opts, std::vector<uint8_t> *out)
{
    std::vector<uint8_t> telemetry;

    if(!out) {
        return false;
    }
    out->clear();
    append_uint32_field(&telemetry, 1U, opts.telemetry_device_interval_sec);
    append_uint32_field(&telemetry, 2U,
                        opts.telemetry_environment_interval_sec);
    append_bool_field(&telemetry, 3U,
                      opts.environment_telemetry_enabled);
    append_bool_field(&telemetry, 4U, true);
    append_bool_field(&telemetry, 14U, opts.telemetry_enabled);
    append_bytes_field(out, 6U, telemetry);
    return true;
}

static bool encode_phoneapi_module_config_default(
    uint32_t module_config_type, std::vector<uint8_t> *out)
{
    std::vector<uint8_t> module;
    uint32_t module_field = module_config_type + 1U;

    if(!out || module_config_type > 16U) {
        return false;
    }
    out->clear();
    switch(module_config_type) {
    case 0U:
        append_bool_field(&module, 1U, false);
        append_bool_field(&module, 5U, true);
        append_string_field(&module, 8U, "msh", 15U);
        break;
    case 1U:
        append_bool_field(&module, 1U, false);
        append_uint32_field(&module, 5U, 0U);
        append_uint32_field(&module, 7U, 0U);
        break;
    case 2U:
        append_bool_field(&module, 1U, false);
        append_uint32_field(&module, 2U, 1000U);
        break;
    case 9U:
        append_bool_field(&module, 1U, false);
        append_uint32_field(&module, 2U, 14400U);
        append_bool_field(&module, 3U, false);
        break;
    case 10U:
        append_bool_field(&module, 1U, false);
        append_uint32_field(&module, 2U, 10U);
        break;
    case 16U:
        append_uint32_field(&module, 1U, 0U);
        append_uint32_field(&module, 11U, 3600U);
        break;
    default:
        break;
    }
    append_bytes_field(out, module_field, module);
    return true;
}

static bool encode_phoneapi_module_config_by_type(
    const probe_options_t &opts, uint32_t module_config_type,
    std::vector<uint8_t> *out)
{
    if(module_config_type == 5U) {
        return encode_phoneapi_module_config_telemetry(opts, out);
    }
    return encode_phoneapi_module_config_default(module_config_type, out);
}

static std::string base64url_encode_no_pad(const std::vector<uint8_t> &data)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;

    out.reserve(((data.size() + 2U) / 3U) * 4U);
    while(i + 3U <= data.size()) {
        uint32_t v = ((uint32_t)data[i] << 16U) |
                     ((uint32_t)data[i + 1U] << 8U) |
                     (uint32_t)data[i + 2U];
        out.push_back(table[(v >> 18U) & 0x3fU]);
        out.push_back(table[(v >> 12U) & 0x3fU]);
        out.push_back(table[(v >> 6U) & 0x3fU]);
        out.push_back(table[v & 0x3fU]);
        i += 3U;
    }
    if(i < data.size()) {
        uint32_t v = (uint32_t)data[i] << 16U;
        out.push_back(table[(v >> 18U) & 0x3fU]);
        if(i + 1U < data.size()) {
            v |= (uint32_t)data[i + 1U] << 8U;
            out.push_back(table[(v >> 12U) & 0x3fU]);
            out.push_back(table[(v >> 6U) & 0x3fU]);
        } else {
            out.push_back(table[(v >> 12U) & 0x3fU]);
        }
    }
    return out;
}

static bool encode_meshtastic_channelset_settings(const probe_options_t &opts,
                                                  std::vector<uint8_t> *out)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> psk;

    if(!out || !parse_psk(opts.psk, &key)) {
        return false;
    }
    out->clear();
    if(key.empty()) {
        psk.clear();
    } else if(key.size() == sizeof(default_psk) &&
              memcmp(key.data(), default_psk, sizeof(default_psk)) == 0) {
        psk.push_back(1U);
    } else {
        psk = key;
    }
    append_bytes_field(out, 2U, psk);
    if(!opts.channel_name.empty()) {
        append_string_field(out, 3U, opts.channel_name, 12U);
    }
    return true;
}

static bool encode_meshtastic_channelset_lora(const probe_options_t &opts,
                                              std::vector<uint8_t> *out)
{
    uint32_t region = phoneapi_region_enum(
        opts.resolved_region.empty() ? opts.region : opts.resolved_region);
    uint32_t preset = phoneapi_preset_enum(
        opts.resolved_preset.empty() ? opts.preset : opts.resolved_preset);

    if(!out) {
        return false;
    }
    out->clear();
    append_bool_field(out, 1U, true);
    append_uint32_field(out, 2U, preset);
    append_uint32_field(out, 7U, region);
    append_uint32_field(out, 8U, opts.hop_limit);
    append_bool_field(out, 9U, true);
    if(opts.profile.power > 0) {
        append_uint32_field(out, 10U, (uint32_t)opts.profile.power);
    }
    if(opts.resolved_slot > 0U) {
        append_uint32_field(out, 11U, opts.resolved_slot);
    }
    return !out->empty();
}

static bool encode_meshtastic_channelset(const probe_options_t &opts,
                                         std::vector<uint8_t> *out)
{
    std::vector<uint8_t> settings;
    std::vector<uint8_t> lora;

    if(!out ||
       !encode_meshtastic_channelset_settings(opts, &settings) ||
       !encode_meshtastic_channelset_lora(opts, &lora)) {
        return false;
    }
    out->clear();
    append_bytes_field(out, 1U, settings);
    append_bytes_field(out, 2U, lora);
    return !out->empty();
}

static std::string meshtastic_channel_url(const probe_options_t &opts)
{
    std::vector<uint8_t> channel_set;

    if(!encode_meshtastic_channelset(opts, &channel_set)) {
        return std::string();
    }
    return std::string("https://meshtastic.org/e/#") +
           base64url_encode_no_pad(channel_set);
}

static bool encode_phoneapi_channel_at(const probe_options_t &opts,
                                       uint32_t index,
                                       std::vector<uint8_t> *out)
{
    std::vector<uint8_t> channel;
    std::vector<uint8_t> settings;
    std::vector<uint8_t> module_settings;
    std::vector<uint8_t> key;
    const mesh_channel_slot_t *slot = nullptr;
    std::string channel_name;
    std::string psk;
    uint32_t role = MESHTASTIC_CHANNEL_ROLE_DISABLED;

    if(!out || index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        return false;
    }
    out->clear();
    slot = &opts.channels[index];
    if(slot->configured) {
        role = slot->role;
    } else if(index == opts.primary_channel_index) {
        role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
    }
    append_uint32_field(&channel, 1U, index);
    if(role == MESHTASTIC_CHANNEL_ROLE_DISABLED) {
        append_uint32_field(&channel, 3U, MESHTASTIC_CHANNEL_ROLE_DISABLED);
        *out = channel;
        return true;
    }
    channel_name = mesh_channel_slot_name(opts, index);
    psk = mesh_channel_slot_psk(opts, index);
    if(!parse_psk(psk, &key)) {
        (void)parse_psk("default", &key);
    }
    if(!key.empty()) {
        append_bytes_field(&settings, 2U, key);
    }
    if(!channel_name.empty()) {
        append_string_field(&settings, 3U, channel_name, 31U);
    }
    if(slot->uplink_enabled) {
        append_bool_field(&settings, 5U, true);
    }
    if(slot->downlink_enabled) {
        append_bool_field(&settings, 6U, true);
    }
    if(slot->has_position_precision) {
        append_uint32_field(&module_settings, 1U, slot->position_precision);
    }
    if(slot->is_muted) {
        append_bool_field(&module_settings, 2U, true);
    }
    if(!module_settings.empty()) {
        append_bytes_field(&settings, 7U, module_settings);
    }
    append_bytes_field(&channel, 2U, settings);
    append_uint32_field(&channel, 3U, role);
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

static std::string phoneapi_cached_node_long_name(const mesh_node_entry_t &node)
{
    char fallback[24];

    if(node.long_name[0]) {
        return std::string(node.long_name);
    }
    snprintf(fallback, sizeof(fallback), "node-%04x",
             (unsigned)(node.node & 0xffffU));
    return std::string(fallback);
}

static bool encode_phoneapi_cached_user_proto(const mesh_node_entry_t &node,
                                              std::vector<uint8_t> *out)
{
    char id[16];
    std::string long_name;
    std::string short_name;

    if(!out || node.node == 0U) {
        return false;
    }
    long_name = mesh_clean_text(phoneapi_cached_node_long_name(node));
    if(long_name.empty()) {
        long_name = "node";
    }
    if(node.short_name[0]) {
        short_name = mesh_clean_text(node.short_name);
    }
    if(short_name.empty()) {
        short_name = make_short_node_name(long_name);
    }
    out->clear();
    snprintf(id, sizeof(id), "!%08x", node.node);
    append_string_field(out, 1U, id, 15U);
    append_string_field(out, 2U, long_name, 39U);
    append_string_field(out, 3U, short_name, 4U);
    if(node.hw_model >= 0) {
        append_uint32_field(out, 5U, (uint32_t)node.hw_model);
    }
    if(node.has_public_key) {
        append_bytes_field(out, 8U, node.public_key,
                           MESHTASTIC_CURVE25519_KEY_LEN);
    }
    return true;
}

static bool encode_phoneapi_cached_position_proto(const mesh_node_entry_t &node,
                                                  std::vector<uint8_t> *out)
{
    if(!out || !node.has_position) {
        return false;
    }
    out->clear();
    append_sfixed32_field(out, 1U, node.latitude_i);
    append_sfixed32_field(out, 2U, node.longitude_i);
    if(node.has_altitude) {
        append_uint32_field(out, 3U, (uint32_t)node.altitude_m);
    }
    if(node.position_timestamp != 0U) {
        append_varint(out, (7U << 3U) | 5U);
        append_fixed32(out, node.position_timestamp);
    }
    if(node.has_ground_speed) {
        append_uint32_field(out, 15U, node.ground_speed_cms);
    }
    if(node.has_ground_track) {
        append_uint32_field(out, 16U, node.ground_track_1e5);
    }
    if(node.sats_in_view != 0U) {
        append_uint32_field(out, 19U, node.sats_in_view);
    }
    if(node.precision_bits != 0U) {
        append_uint32_field(out, 23U, node.precision_bits);
    }
    return !out->empty();
}

static bool encode_phoneapi_cached_device_metrics_proto(
    const mesh_node_entry_t &node, std::vector<uint8_t> *out)
{
    if(!out || !node.has_device_metrics) {
        return false;
    }
    out->clear();
    if(node.has_battery_level) {
        append_uint32_field(out, 1U, node.battery_level);
    }
    if(node.has_device_voltage) {
        append_float_field(out, 2U, node.device_voltage);
    }
    if(node.has_channel_utilization) {
        append_float_field(out, 3U, node.channel_utilization);
    }
    if(node.has_air_util_tx) {
        append_float_field(out, 4U, node.air_util_tx);
    }
    if(node.uptime_seconds != 0U) {
        append_uint32_field(out, 5U, node.uptime_seconds);
    }
    return !out->empty();
}

static uint32_t phoneapi_node_last_heard_epoch(const mesh_node_entry_t &node)
{
    return mesh_node_last_seen_epoch(node);
}

static bool encode_phoneapi_cached_node_info(const mesh_node_entry_t &node,
                                             std::vector<uint8_t> *out)
{
    std::vector<uint8_t> user;
    std::vector<uint8_t> position;
    std::vector<uint8_t> metrics;

    if(!out || node.node == 0U ||
       !encode_phoneapi_cached_user_proto(node, &user)) {
        return false;
    }
    out->clear();
    append_uint32_field(out, 1U, node.node);
    append_bytes_field(out, 2U, user);
    if(encode_phoneapi_cached_position_proto(node, &position)) {
        append_bytes_field(out, 3U, position);
    }
    append_float_field(out, 4U, node.snr);
    append_varint(out, (5U << 3U) | 5U);
    append_fixed32(out, phoneapi_node_last_heard_epoch(node));
    if(encode_phoneapi_cached_device_metrics_proto(node, &metrics)) {
        append_bytes_field(out, 6U, metrics);
    }
    if(node.has_channel && node.channel_index != 0U) {
        append_uint32_field(out, 7U, node.channel_index);
    }
    if(node.has_hops_away) {
        append_uint32_field(out, 9U, node.hops_away);
    }
    if(node.is_favorite) {
        append_bool_field(out, 10U, true);
    }
    if(node.is_ignored) {
        append_bool_field(out, 11U, true);
    }
    if(node.is_muted) {
        append_bool_field(out, 13U, true);
    }
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
static const char *mesh_routing_error_name(uint32_t error_reason);
static uint32_t mesh_prng_u32(uint32_t salt);
static bool mesh_portnum_uses_pki_direct(uint32_t portnum);
static bool build_mesh_pki_direct_data_frame(const probe_options_t &opts,
                                             uint32_t to_node,
                                             uint32_t portnum,
                                             const std::vector<uint8_t> &payload,
                                             uint32_t request_id,
                                             uint32_t reply_id,
                                             bool want_response,
                                             uint32_t data_dest,
                                             bool request_ack,
                                             bool phoneapi_origin,
                                             const char *summary_kind,
                                             tx_frame_t *frame);
static bool decode_data_proto(const uint8_t *data, size_t len,
                              mesh_data_proto_t *decoded);
static bool decode_position_proto(const std::vector<uint8_t> &payload,
                                  mesh_position_info_t *position);
static bool phoneapi_send_from_payload(int fd, uint32_t field,
                                       const std::vector<uint8_t> &payload,
                                       const char *label);
static bool phoneapi_notify_routing_result(uint32_t from_node,
                                           uint32_t request_id,
                                           uint32_t error_reason,
                                           const char *reason);

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
    bool get_canned_message_module_messages_request = false;
    bool get_device_metadata_request = false;
    bool get_ringtone_request = false;
    bool get_device_connection_status_request = false;
    bool begin_edit_settings = false;
    bool commit_edit_settings = false;
    bool remove_fixed_position = false;
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
    bool has_set_canned_message_module_messages = false;
    std::string set_canned_message_module_messages;
    bool has_set_ringtone_message = false;
    std::string set_ringtone_message;
    bool has_set_fixed_position = false;
    mesh_position_info_t set_fixed_position;
    bool has_remove_by_nodenum = false;
    uint32_t remove_by_nodenum = 0;
    bool has_set_favorite_node = false;
    uint32_t set_favorite_node = 0;
    bool has_remove_favorite_node = false;
    uint32_t remove_favorite_node = 0;
    bool has_set_ignored_node = false;
    uint32_t set_ignored_node = 0;
    bool has_remove_ignored_node = false;
    uint32_t remove_ignored_node = 0;
    bool has_toggle_muted_node = false;
    uint32_t toggle_muted_node = 0;
    bool nodedb_reset = false;
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
        } else if(field == 10U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->get_canned_message_module_messages_request = value != 0U;
        } else if(field == 12U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->get_device_metadata_request = value != 0U;
        } else if(field == 14U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->get_ringtone_request = value != 0U;
        } else if(field == 16U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->get_device_connection_status_request = value != 0U;
        } else if(field == 43U && wire == 5U &&
                  pos + 4U <= payload.size()) {
            out->set_time_only = get_le32(payload.data() + pos);
            out->has_set_time_only = true;
            pos += 4U;
        } else if(field == 64U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->begin_edit_settings = value != 0U;
        } else if(field == 65U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->commit_edit_settings = value != 0U;
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
        } else if(field == 36U && wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            out->set_canned_message_module_messages.assign(
                (const char *)payload.data() + pos, l);
            pos += l;
            out->has_set_canned_message_module_messages = true;
        } else if(field == 37U && wire == 2U) {
            uint32_t l;
            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            out->set_ringtone_message.assign((const char *)payload.data() +
                                             pos, l);
            pos += l;
            out->has_set_ringtone_message = true;
        } else if(field == 38U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->remove_by_nodenum)) {
                return false;
            }
            out->has_remove_by_nodenum = true;
        } else if(field == 39U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->set_favorite_node)) {
                return false;
            }
            out->has_set_favorite_node = true;
        } else if(field == 40U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->remove_favorite_node)) {
                return false;
            }
            out->has_remove_favorite_node = true;
        } else if(field == 41U && wire == 2U) {
            std::vector<uint8_t> position;
            uint32_t l;

            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            position.assign(payload.begin() + (long)pos,
                            payload.begin() + (long)(pos + l));
            pos += l;
            if(!decode_position_proto(position, &out->set_fixed_position)) {
                return false;
            }
            out->has_set_fixed_position = true;
        } else if(field == 42U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->remove_fixed_position = value != 0U;
        } else if(field == 47U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->set_ignored_node)) {
                return false;
            }
            out->has_set_ignored_node = true;
        } else if(field == 48U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->remove_ignored_node)) {
                return false;
            }
            out->has_remove_ignored_node = true;
        } else if(field == 49U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->toggle_muted_node)) {
                return false;
            }
            out->has_toggle_muted_node = true;
        } else if(field == 100U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->nodedb_reset = value != 0U;
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
        } else if(field == 3U && wire == 0U) {
            if(!read_varint(packet.data(), packet.size(), &pos,
                            &found.channel_index)) {
                return false;
            }
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
    bool has_index = false;
    bool has_role = false;
    bool has_name = false;
    bool has_psk = false;
    bool has_uplink_enabled = false;
    bool has_downlink_enabled = false;
    bool has_position_precision = false;
    bool has_is_muted = false;
    uint32_t index = 0U;
    uint32_t role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
    bool uplink_enabled = false;
    bool downlink_enabled = false;
    uint32_t position_precision = 0U;
    bool is_muted = false;
    std::string name;
    std::string psk;
} phoneapi_channel_update_t;

typedef struct {
    bool has_lora = false;
    bool has_position = false;
    bool has_telemetry = false;
    bool has_region = false;
    bool has_preset = false;
    bool has_hop_limit = false;
    bool has_tx_power = false;
    bool has_channel_num = false;
    bool has_position_enabled = false;
    bool has_fixed_position = false;
    bool has_position_interval = false;
    bool has_telemetry_enabled = false;
    bool has_environment_telemetry_enabled = false;
    bool has_telemetry_device_interval = false;
    bool has_telemetry_environment_interval = false;
    std::string region;
    std::string preset;
    uint32_t hop_limit = 0;
    int32_t tx_power = 0;
    uint32_t channel_num = 0;
    bool position_enabled = false;
    bool fixed_position = false;
    uint32_t position_interval_sec = 0;
    bool telemetry_enabled = false;
    bool environment_telemetry_enabled = false;
    uint32_t telemetry_device_interval_sec = 0;
    uint32_t telemetry_environment_interval_sec = 0;
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
    return mesh_psk_bytes_to_text(psk);
}

static bool phoneapi_parse_channel_module_settings(
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
        if(field == 1U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->position_precision = value;
            out->has_position_precision = true;
        } else if(field == 2U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->is_muted = value != 0U;
            out->has_is_muted = true;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
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
        } else if(field == 5U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->uplink_enabled = value != 0U;
            out->has_uplink_enabled = true;
        } else if(field == 6U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->downlink_enabled = value != 0U;
            out->has_downlink_enabled = true;
        } else if(field == 7U && wire == 2U) {
            std::vector<uint8_t> module_settings;
            if(!phoneapi_read_length_delimited(payload, &pos,
                                               &module_settings) ||
               !phoneapi_parse_channel_module_settings(module_settings,
                                                       out)) {
                return false;
            }
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
        if(field == 1U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->index = value;
            out->has_index = true;
        } else if(field == 2U && wire == 2U) {
            std::vector<uint8_t> settings;
            if(!phoneapi_read_length_delimited(payload, &pos, &settings) ||
               !phoneapi_parse_channel_settings(settings, out)) {
                return false;
            }
        } else if(field == 3U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->role = value;
            out->has_role = true;
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
        {15, "UA_868"}, {16, "MY_433"}, {17, "MY_919"},
        {18, "SG_923"},
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
        } else if(field == 11U && wire == 0U) {
            if(!read_varint(payload.data(), payload.size(), &pos,
                            &out->channel_num)) {
                return false;
            }
            out->has_channel_num = out->channel_num > 0U;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_position_config_update(
    const std::vector<uint8_t> &payload, phoneapi_config_update_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    out->has_position = true;
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
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            if(value >= 30U && value <= 86400U) {
                out->position_interval_sec = value;
                out->has_position_interval = true;
            }
        } else if(field == 3U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->fixed_position = value != 0U;
            out->has_fixed_position = true;
        } else if(field == 4U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->position_enabled = value != 0U;
            out->has_position_enabled = true;
        } else if(field == 13U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->position_enabled = value == 1U;
            out->has_position_enabled = true;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_telemetry_module_update(
    const std::vector<uint8_t> &payload, phoneapi_config_update_t *out)
{
    size_t pos = 0;

    if(!out) {
        return false;
    }
    out->has_telemetry = true;
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
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            if(value >= 60U && value <= 86400U) {
                out->telemetry_device_interval_sec = value;
                out->has_telemetry_device_interval = true;
            }
        } else if(field == 2U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            if(value >= 60U && value <= 86400U) {
                out->telemetry_environment_interval_sec = value;
                out->has_telemetry_environment_interval = true;
            }
        } else if(field == 3U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->environment_telemetry_enabled = value != 0U;
            out->has_environment_telemetry_enabled = true;
        } else if(field == 14U && wire == 0U) {
            uint32_t value;
            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            out->telemetry_enabled = value != 0U;
            out->has_telemetry_enabled = true;
        } else if(!phoneapi_proto_skip(payload.data(), payload.size(), &pos,
                                       wire)) {
            return false;
        }
    }
    return true;
}

static bool phoneapi_parse_module_config_update(
    const std::vector<uint8_t> &payload, phoneapi_config_update_t *out)
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
        if(field == 6U && wire == 2U) {
            std::vector<uint8_t> telemetry;
            if(!phoneapi_read_length_delimited(payload, &pos, &telemetry) ||
               !phoneapi_parse_telemetry_module_update(telemetry, out)) {
                return false;
            }
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
        if(field == 2U && wire == 2U) {
            std::vector<uint8_t> position;
            if(!phoneapi_read_length_delimited(payload, &pos, &position) ||
               !phoneapi_parse_position_config_update(position, out)) {
                return false;
            }
        } else if(field == 6U && wire == 2U) {
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

static bool phoneapi_pending_mesh_tx_wants_ack(void)
{
    bool wants_ack = false;

    pthread_mutex_lock(&phoneapi_tx_mutex);
    wants_ack = phoneapi_pending_tx.active && phoneapi_pending_tx.want_ack;
    pthread_mutex_unlock(&phoneapi_tx_mutex);
    return wants_ack;
}

static bool encode_phoneapi_mesh_packet_decoded(const mesh_header_t &header,
                                                const std::vector<uint8_t> &decoded,
                                                uint32_t channel_index,
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
    append_uint32_field(out, 3U, channel_index);
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

    if(fd < 0 || !phoneapi_thread_running || !phoneapi_bridge_has_client()) {
        return false;
    }
    return phoneapi_send_from_payload(fd, field, payload, label);
}

static void phoneapi_notify_mesh_rx(const mesh_header_t &header,
                                    const std::vector<uint8_t> &decoded,
                                    uint32_t channel_index,
                                    float rssi, float snr)
{
    std::vector<uint8_t> packet;

    if(!encode_phoneapi_mesh_packet_decoded(header, decoded, channel_index,
                                            rssi, snr,
                                            &packet)) {
        return;
    }
    (void)phoneapi_send_from_payload_global(2U, packet, "rx_packet");
}

static bool phoneapi_notify_routing_result(uint32_t from_node,
                                           uint32_t request_id,
                                           uint32_t error_reason,
                                           const char *reason)
{
    std::vector<uint8_t> routing_proto;
    std::vector<uint8_t> data_proto;
    std::vector<uint8_t> packet;
    mesh_header_t header;
    uint32_t local_node = phoneapi_opts.from_node;

    if(request_id == 0U || local_node == 0U ||
       !phoneapi_bridge_can_send()) {
        return false;
    }
    if(from_node == 0U) {
        from_node = MESHTASTIC_NODENUM_BROADCAST;
    }
    if(!encode_routing_proto(error_reason, &routing_proto) ||
       !encode_data_proto(MESHTASTIC_ROUTING_APP, routing_proto,
                          request_id, 0U, &data_proto)) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.from = from_node;
    header.to = local_node;
    header.id = (uint32_t)(monotonic_us() & 0xffffffffU) ^
                mesh_prng_u32(request_id);
    if(header.id == 0U) {
        header.id = 1U;
    }
    header.flags = 0U;
    header.channel = 0U;
    header.next_hop = 0U;
    header.relay_node = (uint8_t)(from_node & 0xffU);

    if(!encode_phoneapi_mesh_packet_decoded(header, data_proto, 0U, 0.0f,
                                            0.0f, &packet)) {
        return false;
    }
    if(phoneapi_send_from_payload_global(2U, packet, "routing_result")) {
        daemon_event("PhoneAPI routing result req=0x%08x from=0x%08x err=%u(%s) reason=%s",
                     request_id, from_node, error_reason,
                     mesh_routing_error_name(error_reason),
                     reason && reason[0] ? reason : "-");
        return true;
    }
    return false;
}

static bool phoneapi_send_local_loopback(int fd, const probe_options_t &opts,
                                         const phoneapi_mesh_tx_t &tx)
{
    std::vector<uint8_t> data_proto;
    std::vector<uint8_t> packet;
    mesh_header_t header;
    uint32_t packet_id = tx.packet_id;

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
    memset(&header, 0, sizeof(header));
    header.from = opts.from_node;
    header.to = opts.from_node;
    header.id = packet_id;
    header.flags = tx.hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK;
    header.channel = (uint8_t)(tx.channel_index & 0xffU);
    if(!encode_phoneapi_mesh_packet_decoded(header, data_proto,
                                            tx.channel_index, 0.0f, 0.0f,
                                            &packet)) {
        return false;
    }
    if(!phoneapi_send_from_payload(fd, 2U, packet, "local_loopback")) {
        return false;
    }
    daemon_event("PhoneAPI local loopback id=0x%08x port=%u payload=%u",
                 packet_id, tx.data.portnum,
                 (unsigned)tx.data.payload.size());
    return true;
}

static void phoneapi_notify_node_update(uint32_t node, const char *reason)
{
    std::vector<uint8_t> payload;

    if(node == 0U || node == phoneapi_opts.from_node) {
        return;
    }
    for(size_t i = 0; i < mesh_node_count; i++) {
        if(mesh_nodes[i].node != node) {
            continue;
        }
        if(!encode_phoneapi_cached_node_info(mesh_nodes[i], &payload)) {
            return;
        }
        if(phoneapi_send_from_payload_global(4U, payload, "node_info_update")) {
            daemon_event("PhoneAPI node update sent node=0x%08x reason=%s",
                         node, reason && reason[0] ? reason : "-");
        }
        return;
    }
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
    usleep(MESHTASTIC_PHONEAPI_FROM_SEND_GAP_US);
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
    case 1U:
        ok = encode_phoneapi_config_position(opts, &config);
        break;
    case 2U:
        ok = encode_phoneapi_config_power(&config);
        break;
    case 3U:
        ok = encode_phoneapi_config_network(&config);
        break;
    case 4U:
        ok = encode_phoneapi_config_display(&config);
        break;
    case 5U:
        ok = encode_phoneapi_config_lora(opts, &config);
        break;
    case 6U:
        ok = encode_phoneapi_config_bluetooth(&config);
        break;
    case 7U:
        ok = encode_phoneapi_config_security(&config);
        break;
    case 8U:
        ok = encode_phoneapi_config_sessionkey(&config);
        break;
    case 9U:
        ok = encode_phoneapi_config_device_ui(&config);
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
    bool ok = true;

    if(!out) {
        return false;
    }
    switch(module_config_type) {
    case 0U:
    case 1U:
    case 2U:
    case 3U:
    case 4U:
    case 5U:
    case 6U:
    case 7U:
    case 8U:
    case 9U:
    case 10U:
    case 11U:
    case 12U:
    case 13U:
    case 14U:
    case 15U:
    case 16U:
        ok = encode_phoneapi_module_config_by_type(opts, module_config_type,
                                                   &module_config);
        break;
    default:
        module_config.clear();
        daemon_event("PhoneAPI local admin module config type %u returns empty",
                     module_config_type);
        break;
    }
    return ok && encode_phoneapi_admin_response_bytes(opts, 8U, module_config,
                                                      out);
}

static bool encode_phoneapi_admin_channel_response(const probe_options_t &opts,
                                                   uint32_t channel_request,
                                                   std::vector<uint8_t> *out)
{
    std::vector<uint8_t> channel;
    uint32_t index = channel_request == 0U ? 0U : channel_request - 1U;

    if(!out || index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS ||
       !encode_phoneapi_channel_at(opts, index, &channel)) {
        return false;
    }
    daemon_event("PhoneAPI local admin channel request=%u index=%u",
                 channel_request, index);
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

static bool encode_phoneapi_admin_connection_status_response(
    const probe_options_t &opts, std::vector<uint8_t> *out)
{
    std::vector<uint8_t> status;

    if(!out || !encode_phoneapi_connection_status(&status)) {
        return false;
    }
    return encode_phoneapi_admin_response_bytes(opts, 17U, status, out);
}

#define K230_PHONE_UI_PREFS_FILE K230_PHONE_UI_PREFS_DIR "/settings.conf"
#define K230_PHONE_UI_PREFS_LOCK K230_PHONE_UI_PREFS_DIR "/settings.conf.lock"
#define K230_MESH_PREF_REGION "meshtastic.region"
#define K230_MESH_PREF_PRESET "meshtastic.preset"
#define K230_MESH_PREF_CHANNEL "meshtastic.channel"
#define K230_MESH_PREF_PSK "meshtastic.psk"
#define K230_MESH_PREF_PRIMARY_CHANNEL "meshtastic.channel.primary"
#define K230_MESH_PREF_CHANNEL_SLOT_PREFIX "meshtastic.channel.slot"
#define K230_MESH_PREF_POWER "meshtastic.power"
#define K230_MESH_PREF_NODE "meshtastic.node"
#define K230_MESH_PREF_FROM "meshtastic.from"
#define K230_MESH_PREF_TO "meshtastic.to"
#define K230_MESH_PREF_HOP "meshtastic.hop"
#define K230_MESH_PREF_ACK "meshtastic.ack"
#define K230_MESH_PREF_REBROADCAST "meshtastic.rebroadcast"
#define K230_MESH_PREF_POSITION "meshtastic.position"
#define K230_MESH_PREF_POSITION_INTERVAL "meshtastic.position_interval"
#define K230_MESH_PREF_FIXED_POSITION "meshtastic.fixed_position"
#define K230_MESH_PREF_FIXED_LATITUDE "meshtastic.fixed_latitude_i"
#define K230_MESH_PREF_FIXED_LONGITUDE "meshtastic.fixed_longitude_i"
#define K230_MESH_PREF_FIXED_ALTITUDE "meshtastic.fixed_altitude_m"
#define K230_MESH_PREF_TELEMETRY "meshtastic.telemetry"
#define K230_MESH_PREF_TELEMETRY_ENV "meshtastic.telemetry_env"
#define K230_MESH_PREF_TELEMETRY_DEVICE_INTERVAL "meshtastic.telemetry_device_interval"
#define K230_MESH_PREF_TELEMETRY_ENV_INTERVAL "meshtastic.telemetry_env_interval"
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
        if(entries->size() >= 192U) {
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

static bool phoneapi_pref_get(const std::vector<phoneapi_pref_entry_t> &entries,
                              const char *key, std::string *out)
{
    if(!key || !out) {
        return false;
    }
    for(size_t i = 0; i < entries.size(); i++) {
        if(entries[i].key == key) {
            *out = entries[i].value;
            return true;
        }
    }
    return false;
}

static void phoneapi_channel_pref_key(char *out, size_t out_len,
                                      uint32_t index, const char *field)
{
    if(!out || out_len == 0U || !field) {
        return;
    }
    snprintf(out, out_len, "%s.%u.%s", K230_MESH_PREF_CHANNEL_SLOT_PREFIX,
             index, field);
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

static bool phoneapi_canned_append_message(std::string *out,
                                           const std::string &message)
{
    std::string clean = mesh_clean_text(message);
    size_t needed;
    size_t available;

    if(!out || clean.empty()) {
        return false;
    }
    needed = clean.size() + (out->empty() ? 0U : 1U);
    if(out->size() + needed > K230_MESH_CANNED_MESSAGES_MAX_BYTES) {
        if(!out->empty()) {
            return false;
        }
        if(clean.size() > K230_MESH_CANNED_MESSAGES_MAX_BYTES) {
            clean.resize(K230_MESH_CANNED_MESSAGES_MAX_BYTES);
        }
    }
    if(!out->empty()) {
        out->push_back('|');
    }
    available = K230_MESH_CANNED_MESSAGES_MAX_BYTES - out->size();
    out->append(clean, 0U, clean.size() < available ? clean.size() : available);
    return true;
}

static bool phoneapi_canned_messages_load(std::string *out)
{
    static const char *defaults[] = {
        "OK",
        "On my way",
        "Need help",
        "At location",
        "Battery low",
        "Signal check",
        "Please repeat",
        "Stand by",
    };
    FILE *fp;
    char line[256];
    size_t count = 0U;

    if(!out) {
        return false;
    }
    out->clear();
    fp = fopen(K230_MESH_CANNED_MESSAGES_FILE, "r");
    if(fp) {
        while(count < K230_MESH_CANNED_MESSAGES_MAX_ITEMS &&
              fgets(line, sizeof(line), fp)) {
            std::string clean;

            line[strcspn(line, "\r\n")] = '\0';
            clean = phoneapi_trim_copy(line);
            if(clean.empty() || clean[0] == '#') {
                continue;
            }
            if(phoneapi_canned_append_message(out, clean)) {
                count++;
            }
        }
        fclose(fp);
    }
    if(count == 0U) {
        for(size_t i = 0U; i < ARRAY_SIZE(defaults) &&
             i < K230_MESH_CANNED_MESSAGES_MAX_ITEMS; i++) {
            if(phoneapi_canned_append_message(out, defaults[i])) {
                count++;
            }
        }
    }
    return true;
}

static bool phoneapi_canned_messages_save(const std::string &messages)
{
    char tmp_path[sizeof(K230_MESH_CANNED_MESSAGES_FILE) + 8];
    std::string clipped = messages;
    FILE *fp;
    size_t start = 0U;
    size_t count = 0U;

    if(clipped.size() > K230_MESH_CANNED_MESSAGES_MAX_BYTES) {
        clipped.resize(K230_MESH_CANNED_MESSAGES_MAX_BYTES);
    }
    if(mkdir(K230_MESH_UI_DIR, 0755) != 0 && errno != EEXIST) {
        daemon_event("Canned messages mkdir failed: %s", strerror(errno));
        return false;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
             K230_MESH_CANNED_MESSAGES_FILE);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        daemon_event("Canned messages save open failed: %s", strerror(errno));
        return false;
    }
    fprintf(fp, "# K230 Meshtastic canned messages\n");
    fprintf(fp, "# One message per line. Empty lines are ignored.\n");
    while(start <= clipped.size() &&
          count < K230_MESH_CANNED_MESSAGES_MAX_ITEMS) {
        size_t end = clipped.find('|', start);
        std::string item;

        if(end == std::string::npos) {
            end = clipped.size();
        }
        item = mesh_clean_text(clipped.substr(start, end - start));
        if(!item.empty()) {
            fprintf(fp, "%s\n", item.c_str());
            count++;
        }
        if(end >= clipped.size()) {
            break;
        }
        start = end + 1U;
    }
    if(fclose(fp) != 0) {
        unlink(tmp_path);
        return false;
    }
    if(rename(tmp_path, K230_MESH_CANNED_MESSAGES_FILE) != 0) {
        unlink(tmp_path);
        daemon_event("Canned messages save rename failed: %s",
                     strerror(errno));
        return false;
    }
    daemon_event("Canned messages saved count=%u bytes=%u",
                 (unsigned)count, (unsigned)clipped.size());
    return true;
}

static bool phoneapi_ringtone_load(std::string *out)
{
    FILE *fp;
    char buf[K230_MESH_RINGTONE_MAX_BYTES + 1U];
    size_t n;

    if(!out) {
        return false;
    }
    out->clear();
    fp = fopen(K230_MESH_RINGTONE_FILE, "r");
    if(!fp) {
        return true;
    }
    n = fread(buf, 1U, K230_MESH_RINGTONE_MAX_BYTES, fp);
    fclose(fp);
    buf[n] = '\0';
    *out = mesh_clean_text(buf);
    return true;
}

static bool phoneapi_ringtone_save(const std::string &ringtone)
{
    char tmp_path[sizeof(K230_MESH_RINGTONE_FILE) + 8];
    std::string clean = mesh_clean_text(ringtone);
    FILE *fp;

    if(clean.size() > K230_MESH_RINGTONE_MAX_BYTES) {
        clean.resize(K230_MESH_RINGTONE_MAX_BYTES);
    }
    if(mkdir(K230_MESH_UI_DIR, 0755) != 0 && errno != EEXIST) {
        daemon_event("Ringtone mkdir failed: %s", strerror(errno));
        return false;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
             K230_MESH_RINGTONE_FILE);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        daemon_event("Ringtone save open failed: %s", strerror(errno));
        return false;
    }
    if(!clean.empty()) {
        fprintf(fp, "%s\n", clean.c_str());
    }
    if(fclose(fp) != 0) {
        unlink(tmp_path);
        return false;
    }
    if(rename(tmp_path, K230_MESH_RINGTONE_FILE) != 0) {
        unlink(tmp_path);
        daemon_event("Ringtone save rename failed: %s", strerror(errno));
        return false;
    }
    daemon_event("Ringtone saved bytes=%u", (unsigned)clean.size());
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
    if(!mesh_config_dir_ensure()) {
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
        snprintf(value, sizeof(value), "%u", opts.primary_channel_index);
        phoneapi_pref_set(&entries, K230_MESH_PREF_PRIMARY_CHANNEL, value);
        for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS; i++) {
            char key[64];
            const mesh_channel_slot_t &slot = opts.channels[i];

            phoneapi_channel_pref_key(key, sizeof(key), i, "role");
            snprintf(value, sizeof(value), "%u", slot.configured ?
                     slot.role :
                     (i == opts.primary_channel_index ?
                      MESHTASTIC_CHANNEL_ROLE_PRIMARY :
                      MESHTASTIC_CHANNEL_ROLE_DISABLED));
            phoneapi_pref_set(&entries, key, value);
            phoneapi_channel_pref_key(key, sizeof(key), i, "name");
            phoneapi_pref_set(&entries, key, mesh_channel_slot_name(opts, i));
            phoneapi_channel_pref_key(key, sizeof(key), i, "psk");
            phoneapi_pref_set(&entries, key, mesh_channel_slot_psk(opts, i));
            phoneapi_channel_pref_key(key, sizeof(key), i, "uplink");
            phoneapi_pref_set(&entries, key,
                              slot.uplink_enabled ? "1" : "0");
            phoneapi_channel_pref_key(key, sizeof(key), i, "downlink");
            phoneapi_pref_set(&entries, key,
                              slot.downlink_enabled ? "1" : "0");
            phoneapi_channel_pref_key(key, sizeof(key), i, "muted");
            phoneapi_pref_set(&entries, key, slot.is_muted ? "1" : "0");
            phoneapi_channel_pref_key(key, sizeof(key), i,
                                      "position_precision");
            snprintf(value, sizeof(value), "%u",
                     slot.has_position_precision ?
                     slot.position_precision : 0U);
            phoneapi_pref_set(&entries, key, value);
        }
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
        phoneapi_pref_set(&entries, K230_MESH_PREF_POSITION,
                          opts.position_enabled ? "1" : "0");
        snprintf(value, sizeof(value), "%u", opts.position_interval_sec);
        phoneapi_pref_set(&entries, K230_MESH_PREF_POSITION_INTERVAL, value);
        phoneapi_pref_set(&entries, K230_MESH_PREF_FIXED_POSITION,
                          opts.fixed_position_enabled ? "1" : "0");
        snprintf(value, sizeof(value), "%d", opts.fixed_position_latitude_i);
        phoneapi_pref_set(&entries, K230_MESH_PREF_FIXED_LATITUDE, value);
        snprintf(value, sizeof(value), "%d", opts.fixed_position_longitude_i);
        phoneapi_pref_set(&entries, K230_MESH_PREF_FIXED_LONGITUDE, value);
        snprintf(value, sizeof(value), "%d", opts.fixed_position_altitude_m);
        phoneapi_pref_set(&entries, K230_MESH_PREF_FIXED_ALTITUDE, value);
        phoneapi_pref_set(&entries, K230_MESH_PREF_TELEMETRY,
                          opts.telemetry_enabled ? "1" : "0");
        phoneapi_pref_set(&entries, K230_MESH_PREF_TELEMETRY_ENV,
                          opts.environment_telemetry_enabled ? "1" : "0");
        snprintf(value, sizeof(value), "%u",
                 opts.telemetry_device_interval_sec);
        phoneapi_pref_set(&entries,
                          K230_MESH_PREF_TELEMETRY_DEVICE_INTERVAL, value);
        snprintf(value, sizeof(value), "%u",
                 opts.telemetry_environment_interval_sec);
        phoneapi_pref_set(&entries, K230_MESH_PREF_TELEMETRY_ENV_INTERVAL,
                          value);
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
            uint32_t index = channel.has_index ? channel.index : 0U;
            uint32_t role = channel.has_role ? channel.role :
                            (index == 0U ? MESHTASTIC_CHANNEL_ROLE_PRIMARY :
                                           MESHTASTIC_CHANNEL_ROLE_SECONDARY);

            if(index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS ||
               role > MESHTASTIC_CHANNEL_ROLE_SECONDARY) {
                ok = false;
                daemon_event("PhoneAPI local admin set_channel invalid index=%u role=%u",
                             index, role);
            } else if(role == MESHTASTIC_CHANNEL_ROLE_DISABLED) {
                mesh_channel_slot_t &slot = opts->channels[index];

                slot = mesh_channel_slot_t();
                slot.configured = false;
                slot.role = MESHTASTIC_CHANNEL_ROLE_DISABLED;
                if(index == opts->primary_channel_index) {
                    opts->primary_channel_index = 0U;
                    opts->channels[0].configured = true;
                    opts->channels[0].role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
                    opts->channel_name = opts->channels[0].name;
                    opts->psk = opts->channels[0].psk.empty() ?
                                std::string("default") : opts->channels[0].psk;
                    *request_reconfigure = true;
                }
                daemon_event("PhoneAPI local admin set_channel index=%u disabled",
                             index);
            } else {
                mesh_channel_slot_t &slot = opts->channels[index];

                slot.configured = true;
                slot.role = role;
                if(channel.has_name) {
                    slot.name = mesh_clean_text(channel.name);
                }
                if(channel.has_psk) {
                    slot.psk = channel.psk;
                }
                if(channel.has_uplink_enabled) {
                    slot.uplink_enabled = channel.uplink_enabled;
                }
                if(channel.has_downlink_enabled) {
                    slot.downlink_enabled = channel.downlink_enabled;
                }
                if(channel.has_position_precision) {
                    slot.position_precision = channel.position_precision;
                    slot.has_position_precision = true;
                }
                if(channel.has_is_muted) {
                    slot.is_muted = channel.is_muted;
                }
                if(slot.psk.empty()) {
                    slot.psk = (index == opts->primary_channel_index) ?
                               opts->psk : std::string("default");
                }
                if(role == MESHTASTIC_CHANNEL_ROLE_PRIMARY) {
                    for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS;
                        i++) {
                        if(i != index && opts->channels[i].configured &&
                           opts->channels[i].role ==
                           MESHTASTIC_CHANNEL_ROLE_PRIMARY) {
                            opts->channels[i].role =
                                MESHTASTIC_CHANNEL_ROLE_SECONDARY;
                        }
                    }
                    opts->primary_channel_index = index;
                    opts->channel_name = slot.name;
                    opts->psk = slot.psk;
                    *request_reconfigure = true;
                } else if(index == opts->primary_channel_index) {
                    slot.role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
                    role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
                }

                daemon_event("PhoneAPI local admin set_channel index=%u role=%u name=%s psk=%s uplink=%s downlink=%s muted=%s",
                             index, role,
                             mesh_channel_slot_name(*opts, index).empty() ?
                             "<preset>" :
                             mesh_channel_slot_name(*opts, index).c_str(),
                             mesh_channel_slot_psk(*opts, index).c_str(),
                             slot.uplink_enabled ? "yes" : "no",
                             slot.downlink_enabled ? "yes" : "no",
                             slot.is_muted ? "yes" : "no");
            }
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
            if(config.has_channel_num) {
                opts->frequency_slot = config.channel_num;
            }
            *request_reconfigure = true;
            daemon_event("PhoneAPI local admin set_config lora region=%s preset=%s hop=%u power=%d slot=%u",
                         opts->region.c_str(), opts->preset.c_str(),
                         opts->hop_limit, opts->profile.power,
                         opts->frequency_slot);
        }
        if(ok && config.has_position) {
            if(config.has_position_enabled) {
                opts->position_enabled = config.position_enabled;
            }
            if(config.has_fixed_position) {
                opts->fixed_position_enabled = config.fixed_position;
            }
            if(config.has_position_interval) {
                opts->position_interval_sec = config.position_interval_sec;
            }
            mesh_next_position_us = opts->position_enabled ?
                monotonic_us() + 5000000ULL : 0ULL;
            nrf9151_gnss_set_state(opts->position_enabled ? "probing" : "off",
                                   opts->position_enabled ? "unavailable" :
                                   "off",
                                   opts->position_enabled ?
                                   "PhoneAPI update" : "Disabled");
            daemon_event("PhoneAPI local admin set_config position enabled=%s fixed=%s interval=%u",
                         opts->position_enabled ? "yes" : "no",
                         opts->fixed_position_enabled ? "yes" : "no",
                         opts->position_interval_sec);
        }
        ok_all = ok_all && ok;
    }

    if(admin.has_set_fixed_position) {
        const mesh_position_info_t &position = admin.set_fixed_position;
        bool ok = position.has_latitude && position.has_longitude;

        if(ok) {
            opts->position_enabled = true;
            opts->fixed_position_enabled = true;
            opts->fixed_position_latitude_i = position.latitude_i;
            opts->fixed_position_longitude_i = position.longitude_i;
            opts->fixed_position_has_altitude = position.has_altitude;
            opts->fixed_position_altitude_m = position.has_altitude ?
                position.altitude_m : 0;
            mesh_next_position_us = monotonic_us() + 2000000ULL;
            mesh_node_update_position(opts->from_node, position);
        }
        daemon_event("PhoneAPI local admin set_fixed_position lat=%.7f lon=%.7f alt=%d ok=%s",
                     position.latitude_i * 1e-7,
                     position.longitude_i * 1e-7,
                     position.has_altitude ? position.altitude_m : 0,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
    }

    if(admin.remove_fixed_position) {
        opts->fixed_position_enabled = false;
        mesh_next_position_us = opts->position_enabled ?
            monotonic_us() + 5000000ULL : 0ULL;
        daemon_event("PhoneAPI local admin remove_fixed_position");
    }

    if(admin.has_set_module_config) {
        phoneapi_config_update_t config;
        bool ok = phoneapi_parse_module_config_update(
                      admin.set_module_config, &config);

        if(ok && config.has_telemetry) {
            uint64_t now = monotonic_us();

            if(config.has_telemetry_enabled) {
                opts->telemetry_enabled = config.telemetry_enabled;
            }
            if(config.has_environment_telemetry_enabled) {
                opts->environment_telemetry_enabled =
                    config.environment_telemetry_enabled;
            }
            if(config.has_telemetry_device_interval) {
                opts->telemetry_device_interval_sec =
                    config.telemetry_device_interval_sec;
            }
            if(config.has_telemetry_environment_interval) {
                opts->telemetry_environment_interval_sec =
                    config.telemetry_environment_interval_sec;
            }
            mesh_next_device_telemetry_us = opts->telemetry_enabled ?
                now + 5000000ULL : 0ULL;
            mesh_next_environment_telemetry_us =
                opts->environment_telemetry_enabled ?
                now + 7000000ULL : 0ULL;
            daemon_event("PhoneAPI local admin set_module_config telemetry device=%s device_interval=%u env=%s env_interval=%u",
                         opts->telemetry_enabled ? "yes" : "no",
                         opts->telemetry_device_interval_sec,
                         opts->environment_telemetry_enabled ? "yes" : "no",
                         opts->telemetry_environment_interval_sec);
        } else {
            daemon_event("PhoneAPI local admin set_module_config unsupported len=%u ok=%s",
                         (unsigned)admin.set_module_config.size(),
                         ok ? "yes" : "no");
        }
        ok_all = ok_all && ok;
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
    if(!encode_phoneapi_mesh_packet_decoded(header, decoded, tx.channel_index,
                                            0.0f, 0.0f, &packet)) {
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
    if(admin.begin_edit_settings) {
        daemon_event("PhoneAPI local admin begin_edit_settings id=0x%08x",
                     tx.packet_id);
        handled = true;
    }
    if(admin.commit_edit_settings) {
        daemon_event("PhoneAPI local admin commit_edit_settings id=0x%08x",
                     tx.packet_id);
        handled = true;
    }
    if(admin.has_set_canned_message_module_messages) {
        bool ok = phoneapi_canned_messages_save(
                      admin.set_canned_message_module_messages);
        daemon_event("PhoneAPI local admin set_canned_messages id=0x%08x bytes=%u ok=%s",
                     tx.packet_id,
                     (unsigned)admin.set_canned_message_module_messages.size(),
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_set_ringtone_message) {
        bool ok = phoneapi_ringtone_save(admin.set_ringtone_message);

        daemon_event("PhoneAPI local admin set_ringtone id=0x%08x bytes=%u ok=%s",
                     tx.packet_id,
                     (unsigned)admin.set_ringtone_message.size(),
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_remove_by_nodenum) {
        bool valid = admin.remove_by_nodenum != 0U;
        bool removed = valid &&
            mesh_node_remove_by_num(admin.remove_by_nodenum);

        daemon_event("PhoneAPI local admin remove_node id=0x%08x node=0x%08x removed=%s valid=%s",
                     tx.packet_id, admin.remove_by_nodenum,
                     removed ? "yes" : "no", valid ? "yes" : "no");
        ok_all = ok_all && valid;
        handled = true;
    }
    if(admin.has_set_favorite_node) {
        bool ok = mesh_node_set_local_flag(admin.set_favorite_node,
                                           true, true, false, false,
                                           false);

        daemon_event("PhoneAPI local admin favorite_node id=0x%08x node=0x%08x ok=%s",
                     tx.packet_id, admin.set_favorite_node,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_remove_favorite_node) {
        bool ok = mesh_node_set_local_flag(admin.remove_favorite_node,
                                           true, false, false, false,
                                           false);

        daemon_event("PhoneAPI local admin unfavorite_node id=0x%08x node=0x%08x ok=%s",
                     tx.packet_id, admin.remove_favorite_node,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_set_ignored_node) {
        bool ok = mesh_node_set_local_flag(admin.set_ignored_node,
                                           false, false, true, true,
                                           false);

        daemon_event("PhoneAPI local admin ignore_node id=0x%08x node=0x%08x ok=%s",
                     tx.packet_id, admin.set_ignored_node,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_remove_ignored_node) {
        bool ok = mesh_node_set_local_flag(admin.remove_ignored_node,
                                           false, false, true, false,
                                           false);

        daemon_event("PhoneAPI local admin unignore_node id=0x%08x node=0x%08x ok=%s",
                     tx.packet_id, admin.remove_ignored_node,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.has_toggle_muted_node) {
        bool ok = mesh_node_set_local_flag(admin.toggle_muted_node,
                                           false, false, false, false,
                                           true);

        daemon_event("PhoneAPI local admin toggle_muted_node id=0x%08x node=0x%08x ok=%s",
                     tx.packet_id, admin.toggle_muted_node,
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.nodedb_reset) {
        size_t before = mesh_node_count;

        mesh_nodedb_reset_preserve_favorites();
        daemon_event("PhoneAPI local admin nodedb_reset id=0x%08x before=%u after=%u",
                     tx.packet_id, (unsigned)before,
                     (unsigned)mesh_node_count);
        handled = true;
    }
    if(admin.has_remove_by_nodenum || admin.has_set_favorite_node ||
       admin.has_remove_favorite_node || admin.has_set_ignored_node ||
       admin.has_remove_ignored_node || admin.has_toggle_muted_node ||
       admin.nodedb_reset) {
        bool save_ok = mesh_nodedb_save();

        daemon_event("PhoneAPI local admin nodedb_write id=0x%08x ok=%s",
                     tx.packet_id, save_ok ? "yes" : "no");
        ok_all = ok_all && save_ok;
    }
    if(admin.has_set_owner || admin.has_set_channel || admin.has_set_config ||
       admin.has_set_module_config || admin.has_set_fixed_position ||
       admin.remove_fixed_position) {
        bool persist_required = admin.has_set_owner || admin.has_set_channel ||
                                admin.has_set_config ||
                                admin.has_set_module_config ||
                                admin.has_set_fixed_position ||
                                admin.remove_fixed_position;
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
    if(admin.get_canned_message_module_messages_request) {
        std::string messages;
        std::vector<uint8_t> canned_bytes;
        std::vector<uint8_t> response;
        bool ok = phoneapi_canned_messages_load(&messages);

        if(ok) {
            canned_bytes.assign(messages.begin(), messages.end());
            ok = encode_phoneapi_admin_response_bytes(runtime_opts, 11U,
                                                      canned_bytes,
                                                      &response) &&
                 phoneapi_send_local_admin_response(fd, tx, response,
                                                    "admin_canned_messages");
        }
        daemon_event("PhoneAPI local admin canned_messages_response id=0x%08x bytes=%u ok=%s",
                     tx.packet_id, (unsigned)messages.size(),
                     ok ? "yes" : "no");
        ok_all = ok_all && ok;
        handled = true;
    }
    if(admin.get_ringtone_request) {
        std::string ringtone;
        std::vector<uint8_t> ringtone_bytes;
        std::vector<uint8_t> response;
        bool ok = phoneapi_ringtone_load(&ringtone);

        if(ok) {
            ringtone_bytes.assign(ringtone.begin(), ringtone.end());
            ok = encode_phoneapi_admin_response_bytes(runtime_opts, 15U,
                                                      ringtone_bytes,
                                                      &response) &&
                 phoneapi_send_local_admin_response(fd, tx, response,
                                                    "admin_ringtone");
        }
        daemon_event("PhoneAPI local admin ringtone_response id=0x%08x bytes=%u ok=%s",
                     tx.packet_id, (unsigned)ringtone.size(),
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
    if(admin.get_device_connection_status_request) {
        std::vector<uint8_t> response;
        bool ok = encode_phoneapi_admin_connection_status_response(
                      runtime_opts, &response) &&
                  phoneapi_send_local_admin_response(fd, tx, response,
                                                     "admin_connection_status");
        daemon_event("PhoneAPI local admin connection_status_response id=0x%08x ok=%s",
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
    usleep(MESHTASTIC_PHONEAPI_FROM_SEND_GAP_US);
    return true;
}

static bool phoneapi_send_nodeinfo_entries(int fd, const probe_options_t &opts,
                                           const char *reason,
                                           uint32_t *sent_count)
{
    std::vector<uint8_t> payload;
    uint32_t sent = 0U;
    uint32_t cached = 0U;
    bool ok = true;

    if(encode_phoneapi_node_info(opts, &payload)) {
        ok = phoneapi_send_from_payload(fd, 4U, payload,
                                        "node_info_local") && ok;
        sent++;
    } else {
        ok = false;
    }

    for(size_t i = 0; i < mesh_node_count; i++) {
        if(mesh_nodes[i].node == 0U || mesh_nodes[i].node == opts.from_node) {
            continue;
        }
        if(encode_phoneapi_cached_node_info(mesh_nodes[i], &payload)) {
            ok = phoneapi_send_from_payload(fd, 4U, payload,
                                            "node_info_cached") && ok;
            sent++;
            cached++;
        } else {
            ok = false;
        }
    }
    if(sent_count) {
        *sent_count = sent;
    }
    daemon_event("PhoneAPI nodeinfo entries reason=%s sent=%u cached=%u ok=%s",
                 reason && reason[0] ? reason : "-",
                 sent, cached, ok ? "yes" : "no");
    return ok;
}

static bool phoneapi_send_module_config_entries(int fd,
                                                const probe_options_t &opts)
{
    std::vector<uint8_t> payload;
    bool ok = true;

    for(uint32_t type = 0U; type <= 16U; type++) {
        char label[32];
        if(!encode_phoneapi_module_config_by_type(opts, type, &payload)) {
            ok = false;
            continue;
        }
        snprintf(label, sizeof(label), "module_config_%u", type);
        ok = phoneapi_send_from_payload(fd, 9U, payload, label) && ok;
    }
    return ok;
}

static bool phoneapi_send_channel_entries(int fd, const probe_options_t &opts)
{
    std::vector<uint8_t> payload;
    bool ok = true;

    for(uint32_t index = 0U; index < MESHTASTIC_PHONEAPI_MAX_CHANNELS;
        index++) {
        char label[32];
        if(!encode_phoneapi_channel_at(opts, index, &payload)) {
            ok = false;
            continue;
        }
        snprintf(label, sizeof(label), "channel_%u", index);
        ok = phoneapi_send_from_payload(fd, 10U, payload, label) && ok;
    }
    return ok;
}

static bool phoneapi_send_config_stage(int fd, const probe_options_t &opts,
                                       uint32_t nonce)
{
    std::vector<uint8_t> payload;
    uint32_t nodeinfo_sent = 0U;
    bool ok = true;

    daemon_event("PhoneAPI config stage requested nonce=%u", nonce);
    ok = encode_phoneapi_my_node_info(opts, phoneapi_nodedb_count(opts.from_node),
                                      &payload) &&
         phoneapi_send_from_payload(fd, 3U, payload, "my_info") && ok;
    ok = encode_phoneapi_metadata(&payload) &&
         phoneapi_send_from_payload(fd, 13U, payload, "metadata") && ok;
    ok = encode_phoneapi_region_presets(&payload) &&
         phoneapi_send_from_payload(fd, 19U, payload, "region_presets") && ok;
    ok = encode_phoneapi_config_lora(opts, &payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_lora") && ok;
    ok = encode_phoneapi_config_device(&payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_device") && ok;
    ok = encode_phoneapi_config_position(opts, &payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_position") && ok;
    ok = encode_phoneapi_config_bluetooth(&payload) &&
         phoneapi_send_from_payload(fd, 5U, payload, "config_bluetooth") && ok;
    ok = phoneapi_send_module_config_entries(fd, opts) && ok;
    ok = phoneapi_send_channel_entries(fd, opts) && ok;
    ok = phoneapi_send_nodeinfo_entries(fd, opts, "config",
                                        &nodeinfo_sent) && ok;
    daemon_event("PhoneAPI config stage nodeinfo_sent=%u", nodeinfo_sent);
    ok = phoneapi_send_config_complete(fd, nonce) && ok;
    return ok;
}

static bool phoneapi_send_nodeinfo_stage(int fd, const probe_options_t &opts,
                                         uint32_t nonce)
{
    uint32_t nodeinfo_sent = 0U;
    bool ok = true;

    daemon_event("PhoneAPI nodeinfo stage requested nonce=%u", nonce);
    ok = phoneapi_send_nodeinfo_entries(fd, opts, "nodeinfo",
                                        &nodeinfo_sent) && ok;
    daemon_event("PhoneAPI nodeinfo stage nodeinfo_sent=%u", nodeinfo_sent);
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
            } else if(tx.to_node == phoneapi_opts.from_node &&
                      tx.to_node != 0U) {
                bool ok = phoneapi_send_local_loopback(fd, phoneapi_opts, tx);
                daemon_event("PhoneAPI ToRadio local loopback handled len=%u id=0x%08x ok=%s",
                             (unsigned)msg.packet_len, tx.packet_id,
                             ok ? "yes" : "no");
                (void)phoneapi_send_queue_status(
                    fd, tx.packet_id,
                    ok ? MESHTASTIC_ERRNO_SHOULD_RELEASE : 1U,
                    ok ? 1U : 0U,
                    ok ? "local-loopback" : "local-loopback-failed");
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

static void phoneapi_process_uart_record(int fd, const std::string &raw_line)
{
    std::string line = raw_line;

    while(!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                            isspace((unsigned char)line.back()))) {
        line.pop_back();
    }
    if(line.empty() || line == "OK") {
        return;
    }
    if(line.rfind("+MESH:PASSKEY,", 0) == 0) {
        const char *p = line.c_str() + strlen("+MESH:PASSKEY,");
        const char *code_start = strchr(p, ',');
        char code[16] = { 0 };

        if(code_start) {
            size_t n = 0;
            code_start++;
            while(code_start[n] && code_start[n] != ',' &&
                  n + 1U < sizeof(code)) {
                code[n] = code_start[n];
                n++;
            }
            code[n] = '\0';
        }
        phoneapi_bridge_set_pairing_code(code);
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_CONNECTED, "pairing");
        phoneapi_mesh_connected_event = true;
        daemon_event("PhoneAPI BLE pairing code %s",
                     code[0] ? code : "invalid");
        return;
    }
    if(line.rfind("+MESH:PAIR,", 0) == 0) {
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_CONNECTED,
                                  line.find(",OK") != std::string::npos ?
                                  "paired" : "pair-failed");
        phoneapi_mesh_connected_event = true;
        daemon_event("PhoneAPI UART %s", line.c_str());
        return;
    }
    if(line.rfind("+MESH:SECURED,", 0) == 0) {
        phoneapi_bridge_set_state(PHONEAPI_BRIDGE_CONNECTED, "secured");
        phoneapi_mesh_connected_event = true;
        daemon_event("PhoneAPI UART %s", line.c_str());
        return;
    }
    if(line.rfind("+MESH:DISCONNECTED", 0) == 0) {
        phoneapi_mesh_connected_event = false;
        phoneapi_bridge_clear_pairing_code();
    }
    if(phoneapi_bridge_status_line(line)) {
        if(line.rfind("+MESH:CONNECTED", 0) == 0) {
            phoneapi_mesh_connected_event = true;
        }
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

static void phoneapi_process_uart_line(int fd, const std::string &raw_line)
{
    std::string line = raw_line;
    unsigned int parts = 0;

    while(!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                            isspace((unsigned char)line.back()))) {
        line.pop_back();
    }
    while(!line.empty()) {
        size_t event_pos = line.find("+MESH:");
        if(event_pos != std::string::npos && event_pos > 0U) {
            std::string prefix = line.substr(0, event_pos);
            phoneapi_process_uart_record(fd, prefix);
            parts++;
            line.erase(0, event_pos);
            continue;
        }

        size_t next_pos = line.find("+MESH:", 1U);
        if(next_pos == std::string::npos) {
            phoneapi_process_uart_record(fd, line);
            parts++;
            break;
        }

        phoneapi_process_uart_record(fd, line.substr(0, next_pos));
        parts++;
        line.erase(0, next_pos);
    }

    if(parts > 1U) {
        daemon_event("PhoneAPI UART split merged line parts=%u",
                     parts);
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
    phoneapi_mesh_connected_event = false;
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
        } else if(field == 8U && wire == 2U) {
            uint32_t l;

            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            if(l == MESHTASTIC_CURVE25519_KEY_LEN) {
                memcpy(found.public_key, payload.data() + pos,
                       MESHTASTIC_CURVE25519_KEY_LEN);
                found.has_public_key = true;
            }
            pos += l;
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
    return found.has_name || found.hw_model >= 0 || found.has_public_key;
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

static bool decode_waypoint_proto(const std::vector<uint8_t> &payload,
                                  mesh_waypoint_info_t *waypoint)
{
    size_t pos = 0;
    mesh_waypoint_info_t found;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if((field == 2U || field == 3U || field == 8U) &&
           wire == 5U && pos + 4U <= payload.size()) {
            uint32_t value = get_le32(payload.data() + pos);
            pos += 4U;
            if(field == 2U) {
                found.latitude_i = (int32_t)value;
                found.has_latitude = true;
            } else if(field == 3U) {
                found.longitude_i = (int32_t)value;
                found.has_longitude = true;
            } else {
                found.icon = value;
            }
        } else if((field == 1U || field == 4U || field == 5U) &&
                  wire == 0U) {
            uint32_t value = 0U;

            if(!read_varint(payload.data(), payload.size(), &pos, &value)) {
                return false;
            }
            if(field == 1U) {
                found.id = value;
                found.has_id = true;
            } else if(field == 4U) {
                found.expire = value;
            } else {
                found.locked_to = value;
            }
        } else if((field == 6U || field == 7U) && wire == 2U) {
            uint32_t l;
            std::string text;
            std::string clean;

            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            text.assign((const char *)payload.data() + pos, l);
            pos += l;
            clean = mesh_clean_text(text);
            if(field == 6U) {
                snprintf(found.name, sizeof(found.name), "%s",
                         clean.c_str());
            } else {
                snprintf(found.description, sizeof(found.description), "%s",
                         clean.c_str());
            }
        } else if(wire == 0U) {
            uint64_t ignored;
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

    if(waypoint) {
        *waypoint = found;
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

static void route_append_node(std::string *out, uint32_t node)
{
    char part[16];

    if(!out) {
        return;
    }
    if(!out->empty()) {
        *out += ">";
    }
    snprintf(part, sizeof(part), "0x%08x", node);
    *out += part;
}

static void route_append_snr(std::string *out, int32_t snr_q4)
{
    char part[16];

    if(!out) {
        return;
    }
    if(!out->empty()) {
        *out += ",";
    }
    snprintf(part, sizeof(part), "%.1f", (double)snr_q4 / 4.0);
    *out += part;
}

static bool decode_route_discovery_proto(const std::vector<uint8_t> &payload,
                                         std::string *summary)
{
    size_t pos = 0;
    std::string route;
    std::string route_back;
    std::string snr_towards;
    std::string snr_back;
    size_t route_count = 0;
    size_t back_count = 0;
    size_t snr_towards_count = 0;
    size_t snr_back_count = 0;

    while(pos < payload.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(payload.data(), payload.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if((field == 1U || field == 3U) && wire == 5U &&
           pos + 4U <= payload.size()) {
            uint32_t node = get_le32(payload.data() + pos);
            pos += 4U;
            if(field == 1U) {
                route_append_node(&route, node);
                route_count++;
            } else {
                route_append_node(&route_back, node);
                back_count++;
            }
        } else if((field == 1U || field == 3U) && wire == 2U) {
            uint32_t l;
            size_t end;

            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            end = pos + l;
            while(pos + 4U <= end) {
                uint32_t node = get_le32(payload.data() + pos);
                pos += 4U;
                if(field == 1U) {
                    route_append_node(&route, node);
                    route_count++;
                } else {
                    route_append_node(&route_back, node);
                    back_count++;
                }
            }
            pos = end;
        } else if((field == 2U || field == 4U) && wire == 0U) {
            uint32_t raw;
            int32_t snr_q4;

            if(!read_varint(payload.data(), payload.size(), &pos, &raw)) {
                return false;
            }
            snr_q4 = mesh_zigzag32_decode(raw);
            if(field == 2U) {
                route_append_snr(&snr_towards, snr_q4);
                snr_towards_count++;
            } else {
                route_append_snr(&snr_back, snr_q4);
                snr_back_count++;
            }
        } else if((field == 2U || field == 4U) && wire == 2U) {
            uint32_t l;
            size_t end;

            if(!read_varint(payload.data(), payload.size(), &pos, &l) ||
               pos + l > payload.size()) {
                return false;
            }
            end = pos + l;
            while(pos < end) {
                uint32_t raw;
                int32_t snr_q4;

                if(!read_varint(payload.data(), end, &pos, &raw)) {
                    return false;
                }
                snr_q4 = mesh_zigzag32_decode(raw);
                if(field == 2U) {
                    route_append_snr(&snr_towards, snr_q4);
                    snr_towards_count++;
                } else {
                    route_append_snr(&snr_back, snr_q4);
                    snr_back_count++;
                }
            }
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
    if(summary) {
        char counts[64];

        snprintf(counts, sizeof(counts),
                 "route_count=%u back_count=%u snr=%u/%u",
                 (unsigned)route_count, (unsigned)back_count,
                 (unsigned)snr_towards_count, (unsigned)snr_back_count);
        *summary = counts;
        if(!route.empty()) {
            *summary += " route=" + route;
        }
        if(!route_back.empty()) {
            *summary += " back=" + route_back;
        }
        if(!snr_towards.empty()) {
            *summary += " snr=" + snr_towards;
        }
        if(!snr_back.empty()) {
            *summary += " snr_back=" + snr_back;
        }
    }
    return route_count > 0U || back_count > 0U ||
           snr_towards_count > 0U || snr_back_count > 0U;
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

static bool mesh_node_copy_public_key(uint32_t node,
                                      uint8_t out[MESHTASTIC_CURVE25519_KEY_LEN])
{
    mesh_node_entry_t *entry;

    if(!out || node == 0U) {
        return false;
    }
    if(node == phoneapi_opts.from_node && mesh_pki_identity.ready) {
        memcpy(out, mesh_pki_identity.public_key,
               MESHTASTIC_CURVE25519_KEY_LEN);
        return true;
    }
    entry = mesh_node_find(node);
    if(!entry || !entry->has_public_key) {
        return false;
    }
    memcpy(out, entry->public_key, MESHTASTIC_CURVE25519_KEY_LEN);
    return true;
}

static bool mesh_pki_shared_key(
    const uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN],
    uint8_t shared_key[SHA256_DIGEST_LENGTH])
{
    EVP_PKEY *local = nullptr;
    EVP_PKEY *remote = nullptr;
    EVP_PKEY_CTX *ctx = nullptr;
    uint8_t shared[MESHTASTIC_CURVE25519_KEY_LEN] = {0};
    size_t shared_len = sizeof(shared);
    bool ok = false;

    if(!mesh_pki_identity.ready || !remote_public || !shared_key) {
        return false;
    }
    local = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                         mesh_pki_identity.private_key,
                                         MESHTASTIC_CURVE25519_KEY_LEN);
    remote = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                         remote_public,
                                         MESHTASTIC_CURVE25519_KEY_LEN);
    if(!local || !remote) {
        goto out;
    }
    ctx = EVP_PKEY_CTX_new(local, nullptr);
    if(!ctx) {
        goto out;
    }
    if(EVP_PKEY_derive_init(ctx) != 1 ||
       EVP_PKEY_derive_set_peer(ctx, remote) != 1 ||
       EVP_PKEY_derive(ctx, shared, &shared_len) != 1 ||
       shared_len != MESHTASTIC_CURVE25519_KEY_LEN) {
        goto out;
    }
    SHA256(shared, shared_len, shared_key);
    ok = true;

out:
    memset(shared, 0, sizeof(shared));
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(remote);
    EVP_PKEY_free(local);
    return ok;
}

static void mesh_pki_init_nonce(uint8_t nonce[MESHTASTIC_PKC_NONCE_LEN],
                                uint32_t from_node,
                                uint32_t packet_id,
                                uint32_t extra_nonce)
{
    memset(nonce, 0, MESHTASTIC_PKC_NONCE_LEN);
    put_le32(nonce, packet_id);
    put_le32(nonce + 4U, 0U);
    if(extra_nonce != 0U) {
        put_le32(nonce + 4U, extra_nonce);
    }
    put_le32(nonce + 8U, from_node);
}

static bool mesh_pki_decrypt_payload(
    const uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN],
    uint32_t from_node,
    uint32_t packet_id,
    const std::vector<uint8_t> &encrypted_payload,
    std::vector<uint8_t> *plain)
{
    EVP_CIPHER_CTX *ctx = nullptr;
    uint8_t key[SHA256_DIGEST_LENGTH] = {0};
    uint8_t nonce[MESHTASTIC_PKC_NONCE_LEN];
    uint32_t extra_nonce;
    const uint8_t *ciphertext;
    const uint8_t *auth;
    size_t cipher_len;
    int out_len = 0;
    bool ok = false;

    if(!plain || encrypted_payload.size() <= MESHTASTIC_PKC_OVERHEAD ||
       !mesh_pki_shared_key(remote_public, key)) {
        return false;
    }
    cipher_len = encrypted_payload.size() - MESHTASTIC_PKC_OVERHEAD;
    ciphertext = encrypted_payload.data();
    auth = encrypted_payload.data() + cipher_len;
    extra_nonce = get_le32(auth + MESHTASTIC_PKC_TAG_LEN);
    mesh_pki_init_nonce(nonce, from_node, packet_id, extra_nonce);

    plain->assign(cipher_len, 0U);
    ctx = EVP_CIPHER_CTX_new();
    if(!ctx) {
        goto out;
    }
    if(EVP_DecryptInit_ex(ctx, EVP_aes_256_ccm(), nullptr, nullptr,
                          nullptr) != 1 ||
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN,
                           MESHTASTIC_PKC_NONCE_LEN, nullptr) != 1 ||
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG,
                           MESHTASTIC_PKC_TAG_LEN, (void *)auth) != 1 ||
       EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1 ||
       EVP_DecryptUpdate(ctx, nullptr, &out_len, nullptr,
                         (int)cipher_len) != 1 ||
       EVP_DecryptUpdate(ctx, plain->data(), &out_len, ciphertext,
                         (int)cipher_len) != 1) {
        goto out;
    }
    plain->resize((size_t)out_len);
    ok = true;

out:
    memset(key, 0, sizeof(key));
    EVP_CIPHER_CTX_free(ctx);
    if(!ok && plain) {
        plain->clear();
    }
    return ok;
}

static bool mesh_pki_encrypt_payload(
    const uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN],
    uint32_t from_node,
    uint32_t packet_id,
    const std::vector<uint8_t> &plain,
    std::vector<uint8_t> *encrypted_payload)
{
    EVP_CIPHER_CTX *ctx = nullptr;
    uint8_t key[SHA256_DIGEST_LENGTH] = {0};
    uint8_t nonce[MESHTASTIC_PKC_NONCE_LEN];
    uint32_t extra_nonce;
    uint8_t tag[MESHTASTIC_PKC_TAG_LEN] = {0};
    int out_len = 0;
    bool ok = false;

    if(!encrypted_payload || plain.empty() ||
       plain.size() + MESHTASTIC_PKC_OVERHEAD >
           (MESHTASTIC_MAX_LORA_PAYLOAD_LEN - MESHTASTIC_HEADER_LENGTH) ||
       !mesh_pki_shared_key(remote_public, key)) {
        return false;
    }
    if(RAND_bytes((uint8_t *)&extra_nonce, sizeof(extra_nonce)) != 1) {
        extra_nonce = (uint32_t)(monotonic_us() & 0xffffffffU) ^
                      mesh_prng_u32(packet_id);
    }
    mesh_pki_init_nonce(nonce, from_node, packet_id, extra_nonce);
    encrypted_payload->assign(plain.size() + MESHTASTIC_PKC_OVERHEAD, 0U);

    ctx = EVP_CIPHER_CTX_new();
    if(!ctx) {
        goto out;
    }
    if(EVP_EncryptInit_ex(ctx, EVP_aes_256_ccm(), nullptr, nullptr,
                          nullptr) != 1 ||
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN,
                           MESHTASTIC_PKC_NONCE_LEN, nullptr) != 1 ||
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG,
                           MESHTASTIC_PKC_TAG_LEN, nullptr) != 1 ||
       EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1 ||
       EVP_EncryptUpdate(ctx, nullptr, &out_len, nullptr,
                         (int)plain.size()) != 1 ||
       EVP_EncryptUpdate(ctx, encrypted_payload->data(), &out_len,
                         plain.data(), (int)plain.size()) != 1 ||
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_GET_TAG,
                           MESHTASTIC_PKC_TAG_LEN, tag) != 1) {
        goto out;
    }
    memcpy(encrypted_payload->data() + plain.size(), tag, sizeof(tag));
    put_le32(encrypted_payload->data() + plain.size() +
             MESHTASTIC_PKC_TAG_LEN, extra_nonce);
    ok = true;

out:
    memset(key, 0, sizeof(key));
    EVP_CIPHER_CTX_free(ctx);
    if(!ok && encrypted_payload) {
        encrypted_payload->clear();
    }
    return ok;
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

static const char *mesh_routing_error_name(uint32_t error_reason)
{
    switch(error_reason) {
    case MESHTASTIC_ROUTING_ERROR_NONE:
        return "NONE";
    case MESHTASTIC_ROUTING_ERROR_NO_ROUTE:
        return "NO_ROUTE";
    case MESHTASTIC_ROUTING_ERROR_GOT_NAK:
        return "GOT_NAK";
    case MESHTASTIC_ROUTING_ERROR_TIMEOUT:
        return "TIMEOUT";
    case MESHTASTIC_ROUTING_ERROR_NO_INTERFACE:
        return "NO_INTERFACE";
    case MESHTASTIC_ROUTING_ERROR_MAX_RETRANSMIT:
        return "MAX_RETRANSMIT";
    case MESHTASTIC_ROUTING_ERROR_NO_CHANNEL:
        return "NO_CHANNEL";
    case MESHTASTIC_ROUTING_ERROR_TOO_LARGE:
        return "TOO_LARGE";
    case MESHTASTIC_ROUTING_ERROR_NO_RESPONSE:
        return "NO_RESPONSE";
    case MESHTASTIC_ROUTING_ERROR_DUTY_CYCLE_LIMIT:
        return "DUTY_CYCLE_LIMIT";
    default:
        return "UNKNOWN";
    }
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

static uint32_t mesh_position_next_ms(uint64_t now_us)
{
    if(mesh_next_position_us == 0ULL || mesh_next_position_us <= now_us) {
        return 0U;
    }
    return (uint32_t)((mesh_next_position_us - now_us + 999ULL) / 1000ULL);
}

static uint32_t mesh_telemetry_next_ms(uint64_t now_us)
{
    uint64_t next_us = 0ULL;

    if(mesh_next_device_telemetry_us != 0ULL) {
        next_us = mesh_next_device_telemetry_us;
    }
    if(mesh_next_environment_telemetry_us != 0ULL &&
       (next_us == 0ULL || mesh_next_environment_telemetry_us < next_us)) {
        next_us = mesh_next_environment_telemetry_us;
    }
    if(next_us == 0ULL || next_us <= now_us) {
        return 0U;
    }
    return (uint32_t)((next_us - now_us + 999ULL) / 1000ULL);
}

static bool mesh_ack_track_frame(const tx_frame_t &frame)
{
    size_t slot = MESHTASTIC_ACK_RETRY_QUEUE_SIZE;

    if(!frame.want_ack || frame.packet_id == 0U) {
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
        if(mesh_ack_retry_queue[i].frame.phoneapi_origin) {
            (void)phoneapi_notify_routing_result(
                from_node, packet_id, error_reason,
                error_reason == MESHTASTIC_ROUTING_ERROR_NONE ?
                    "air-ack" : "air-nak");
        }
        if(error_reason == MESHTASTIC_ROUTING_ERROR_NONE) {
            mesh_ack_rx_count++;
            (void)daemon_chat_update_tx_status(packet_id, "ack");
            daemon_event("Mesh ACK received id=0x%08x from=0x%08x ack=%lu pending=%u",
                         packet_id, from_node,
                         (unsigned long)mesh_ack_rx_count,
                         mesh_ack_pending_count());
        } else {
            mesh_nak_rx_count++;
            (void)daemon_chat_update_tx_status(packet_id, "nak");
            daemon_event("Mesh NAK received id=0x%08x from=0x%08x err=%u(%s) nak=%lu pending=%u",
                         packet_id, from_node, error_reason,
                         mesh_routing_error_name(error_reason),
                         (unsigned long)mesh_nak_rx_count,
                         mesh_ack_pending_count());
        }
        return true;
    }
    return false;
}

static bool mesh_ack_complete_implicit(uint32_t relay_from_node,
                                       const mesh_header_t &header)
{
    for(size_t i = 0; i < MESHTASTIC_ACK_RETRY_QUEUE_SIZE; i++) {
        if(!mesh_ack_retry_queue[i].active ||
           mesh_ack_retry_queue[i].packet_id != header.id ||
           mesh_ack_retry_queue[i].to_node != MESHTASTIC_NODENUM_BROADCAST ||
           mesh_ack_retry_queue[i].frame.channel != header.channel) {
            continue;
        }
        mesh_ack_retry_queue[i].active = false;
        mesh_ack_retry_queue[i].due_us = 0;
        mesh_ack_rx_count++;
        (void)daemon_chat_update_tx_status(header.id, "relayed");
        if(mesh_ack_retry_queue[i].frame.phoneapi_origin) {
            (void)phoneapi_notify_routing_result(
                relay_from_node, header.id, MESHTASTIC_ROUTING_ERROR_NONE,
                "implicit-rebroadcast");
        }
        daemon_event("Mesh implicit ACK id=0x%08x relay=0x%08x ack=%lu pending=%u",
                     header.id, relay_from_node,
                     (unsigned long)mesh_ack_rx_count,
                     mesh_ack_pending_count());
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
    frame->phoneapi_origin = false;
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

static bool build_mesh_data_frame(const probe_options_t &opts,
                                  uint32_t portnum,
                                  const std::vector<uint8_t> &payload,
                                  uint32_t channel_index,
                                  const char *summary_kind,
                                  tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    std::string psk;
    uint8_t channel_hash = 0U;
    mesh_header_t header;
    uint32_t packet_id = opts.packet_id;
    char summary[220];

    if(!frame || payload.size() > MESHTASTIC_DATA_PAYLOAD_LEN ||
       !mesh_resolve_tx_channel(opts, channel_index, &channel_name, &psk,
                                 &key, &channel_hash)) {
        return false;
    }
    if(!meshtastic_node_is_broadcast(opts.to_node)) {
        uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN];

        if(!mesh_node_copy_public_key(opts.to_node, remote_public)) {
            daemon_event("Mesh direct %s skipped target=0x%08x reason=no-public-key",
                         summary_kind && summary_kind[0] ? summary_kind : "data",
                         opts.to_node);
            return false;
        }
        return build_mesh_pki_direct_data_frame(
            opts, opts.to_node, portnum, payload, 0U, 0U, false, 0U,
            opts.want_ack, false,
            summary_kind && summary_kind[0] ? summary_kind : "data", frame);
    }
    if(packet_id == 0U) {
        packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
        if(packet_id == 0U) {
            packet_id = 1U;
        }
    }
    if(!encode_data_proto(portnum, payload, 0U, 0U, &data_proto)) {
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
    if(opts.want_ack && !meshtastic_node_is_broadcast(header.to)) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    header.channel = channel_hash;
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = false;
    frame->phoneapi_origin = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh %s id=0x%08x from=0x%08x to=0x%08x ch=0x%02x name=%s hop=%u ack=%s psk=%s port=%u payload=%u",
             summary_kind && summary_kind[0] ? summary_kind : "data",
             header.id, header.from, header.to, header.channel,
             channel_name.c_str(), opts.hop_limit,
             frame->want_ack ? "on" : "off",
             key.empty() ? "none" : psk.c_str(), portnum,
             (unsigned)payload.size());
    frame->summary = summary;
    return true;
}

static bool build_mesh_frame(const probe_options_t &opts,
                             const std::string &message,
                             uint32_t channel_index,
                             tx_frame_t *frame)
{
    std::vector<uint8_t> text_payload(message.begin(), message.end());

    if(text_payload.size() > MESHTASTIC_DATA_PAYLOAD_LEN) {
        text_payload.resize(MESHTASTIC_DATA_PAYLOAD_LEN);
    }
    return build_mesh_data_frame(opts, MESHTASTIC_TEXT_MESSAGE_APP,
                                 text_payload, channel_index, "text", frame);
}

static bool build_phoneapi_mesh_data_frame(const probe_options_t &opts,
                                           const phoneapi_mesh_tx_t &tx,
                                           tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    std::string psk;
    uint8_t channel_hash = 0U;
    mesh_header_t header;
    uint32_t packet_id = tx.packet_id;
    uint8_t hop_limit = tx.hop_limit != 0U ? tx.hop_limit : opts.hop_limit;
    char summary[220];

    if(!frame) {
        return false;
    }
    if(tx.to_node != 0U && !meshtastic_node_is_broadcast(tx.to_node) &&
       mesh_portnum_uses_pki_direct(tx.data.portnum)) {
        uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN];

        if(mesh_node_copy_public_key(tx.to_node, remote_public)) {
            return build_mesh_pki_direct_data_frame(
                opts, tx.to_node, tx.data.portnum, tx.data.payload,
                tx.data.request_id, tx.data.reply_id,
                tx.data.want_response, 0U, tx.want_ack, true,
                "phoneapi", frame);
        }
        daemon_event("PhoneAPI direct PKI skipped target=0x%08x port=%u reason=no-public-key",
                     tx.to_node, tx.data.portnum);
    }
    if(!mesh_resolve_tx_channel(opts, tx.channel_index, &channel_name, &psk,
                                &key, &channel_hash)) {
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
    if(tx.want_ack && !meshtastic_node_is_broadcast(header.to)) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    header.channel = channel_hash;
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = false;
    frame->phoneapi_origin = true;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "phoneapi mesh id=0x%08x from=0x%08x to=0x%08x slot=%u ch=0x%02x name=%s port=%u payload=%u hop=%u ack=%s",
             header.id, header.from, header.to, tx.channel_index,
             header.channel, channel_name.c_str(), tx.data.portnum,
             (unsigned)tx.data.payload.size(), hop_limit,
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
    frame->phoneapi_origin = false;
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

static bool build_mesh_position_frame(const probe_options_t &opts,
                                      const mesh_position_info_t &position,
                                      tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> position_proto;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    mesh_header_t header;
    uint32_t packet_id;
    char summary[220];

    if(!frame || !parse_psk(opts.psk, &key) ||
       !encode_position_proto(position, opts.position_interval_sec,
                              &position_proto) ||
       !encode_data_proto(MESHTASTIC_POSITION_APP, position_proto, 0, 0,
                          &data_proto)) {
        return false;
    }
    packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
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
    frame->phoneapi_origin = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh position id=0x%08x from=0x%08x ch=0x%02x lat=%.7f lon=%.7f sats=%u",
             header.id, header.from, header.channel,
             position.latitude_i * 1e-7, position.longitude_i * 1e-7,
             position.sats_in_view);
    frame->summary = summary;
    return true;
}

static bool build_mesh_telemetry_frame(const probe_options_t &opts,
                                       const mesh_telemetry_info_t &telemetry,
                                       bool environment,
                                       tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> telemetry_proto;
    std::vector<uint8_t> data_proto;
    std::string channel_name;
    mesh_header_t header;
    uint32_t packet_id;
    std::string summary_text;
    char summary[260];

    if(!frame || !parse_psk(opts.psk, &key) ||
       !encode_telemetry_proto(telemetry, environment, &telemetry_proto) ||
       !encode_data_proto(MESHTASTIC_TELEMETRY_APP, telemetry_proto, 0, 0,
                          &data_proto)) {
        return false;
    }
    packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
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
    frame->phoneapi_origin = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    summary_text = telemetry_summary(telemetry);
    snprintf(summary, sizeof(summary),
             "mesh telemetry id=0x%08x from=0x%08x ch=0x%02x type=%s %s",
             header.id, header.from, header.channel,
             environment ? "environment" : "device", summary_text.c_str());
    frame->summary = summary;
    return true;
}

static const char *mesh_remote_request_name(mesh_remote_request_type_t type)
{
    switch(type) {
    case MESH_REMOTE_REQ_NODEINFO:
        return "nodeinfo";
    case MESH_REMOTE_REQ_POSITION:
        return "position";
    case MESH_REMOTE_REQ_TELEMETRY_DEVICE:
        return "telemetry-device";
    case MESH_REMOTE_REQ_TELEMETRY_ENVIRONMENT:
        return "telemetry-environment";
    case MESH_REMOTE_REQ_TRACEROUTE:
        return "traceroute";
    case MESH_REMOTE_REQ_NEIGHBORINFO:
        return "neighborinfo";
    default:
        return "unknown";
    }
}

static uint32_t mesh_remote_request_alloc_id(void)
{
    uint32_t id = mesh_remote_request_next_id++;

    if(mesh_remote_request_next_id == 0U) {
        mesh_remote_request_next_id = 1U;
    }
    return id ? id : mesh_remote_request_alloc_id();
}

static mesh_remote_request_status_t *mesh_remote_status_find(uint32_t request_id)
{
    if(request_id == 0U) {
        return nullptr;
    }
    for(size_t i = 0; i < mesh_remote_status_count; i++) {
        if(mesh_remote_status_history[i].request_id == request_id) {
            return &mesh_remote_status_history[i];
        }
    }
    return nullptr;
}

static void mesh_remote_status_record(const mesh_remote_request_t &request,
                                      const char *state, const char *detail,
                                      uint32_t airtime_ms,
                                      uint32_t mesh_packet_id = 0U)
{
    mesh_remote_request_status_t *entry;
    uint64_t now_us = monotonic_us();

    entry = mesh_remote_status_find(request.request_id);
    if(!entry) {
        entry = &mesh_remote_status_history[mesh_remote_status_next];
        mesh_remote_status_next =
            (mesh_remote_status_next + 1U) %
            MESHTASTIC_REMOTE_STATUS_HISTORY_MAX;
        if(mesh_remote_status_count < MESHTASTIC_REMOTE_STATUS_HISTORY_MAX) {
            mesh_remote_status_count++;
        }
        memset(entry, 0, sizeof(*entry));
        entry->type = request.type;
        entry->to_node = request.to_node;
        entry->request_id = request.request_id;
        entry->queued_us = now_us;
    }
    entry->updated_us = now_us;
    entry->airtime_ms = airtime_ms;
    if(mesh_packet_id != 0U) {
        entry->mesh_packet_id = mesh_packet_id;
    }
    if(state && strcmp(state, "replied") == 0) {
        entry->replied_us = now_us;
    }
    snprintf(entry->state, sizeof(entry->state), "%s",
             state ? state : "unknown");
    snprintf(entry->detail, sizeof(entry->detail), "%s",
             detail ? detail : "-");
}

static bool mesh_remote_request_type_matches_port(
    mesh_remote_request_type_t type, uint32_t portnum)
{
    switch(type) {
    case MESH_REMOTE_REQ_NODEINFO:
        return portnum == MESHTASTIC_NODEINFO_APP;
    case MESH_REMOTE_REQ_POSITION:
        return portnum == MESHTASTIC_POSITION_APP;
    case MESH_REMOTE_REQ_TELEMETRY_DEVICE:
    case MESH_REMOTE_REQ_TELEMETRY_ENVIRONMENT:
        return portnum == MESHTASTIC_TELEMETRY_APP;
    case MESH_REMOTE_REQ_TRACEROUTE:
        return portnum == MESHTASTIC_TRACEROUTE_APP;
    case MESH_REMOTE_REQ_NEIGHBORINFO:
        return portnum == MESHTASTIC_NEIGHBORINFO_APP;
    default:
        return false;
    }
}

static bool mesh_remote_status_is_waiting(
    const mesh_remote_request_status_t &entry)
{
    return strcmp(entry.state, "queued") == 0 ||
           strcmp(entry.state, "held") == 0 ||
           strcmp(entry.state, "sending") == 0 ||
           strcmp(entry.state, "sent") == 0;
}

static void mesh_remote_status_mark_replied(uint32_t from_node,
                                            uint32_t portnum,
                                            uint32_t response_request_id,
                                            const char *detail)
{
    uint64_t now_us = monotonic_us();
    mesh_remote_request_status_t *candidate = nullptr;

    if(from_node == 0U || mesh_remote_status_count == 0U) {
        return;
    }
    for(size_t n = 0; n < mesh_remote_status_count; n++) {
        size_t idx = (mesh_remote_status_next +
                      MESHTASTIC_REMOTE_STATUS_HISTORY_MAX - 1U - n) %
                     MESHTASTIC_REMOTE_STATUS_HISTORY_MAX;
        mesh_remote_request_status_t *entry = &mesh_remote_status_history[idx];

        if(entry->to_node != from_node ||
           !mesh_remote_request_type_matches_port(entry->type, portnum)) {
            continue;
        }
        if(mesh_remote_status_is_waiting(*entry) &&
           response_request_id != 0U && entry->mesh_packet_id != 0U &&
           response_request_id == entry->mesh_packet_id) {
            candidate = entry;
            break;
        }
        if(!candidate && mesh_remote_status_is_waiting(*entry) &&
           entry->queued_us != 0ULL && now_us >= entry->queued_us &&
           now_us - entry->queued_us < 120000000ULL) {
            candidate = entry;
        }
    }
    if(candidate) {
        mesh_remote_request_t request;

        request.type = candidate->type;
        request.to_node = candidate->to_node;
        request.request_id = candidate->request_id;
        mesh_remote_status_record(request, "replied",
                                  detail && detail[0] ? detail :
                                  "response_received",
                                  candidate->airtime_ms,
                                  candidate->mesh_packet_id);
        daemon_event("Remote request replied id=%u target=0x%08x type=%s request=0x%08x detail=%s",
                     candidate->request_id, candidate->to_node,
                     mesh_remote_request_name(candidate->type),
                     response_request_id,
                     detail && detail[0] ? detail : "response_received");
    }
}

static size_t mesh_remote_status_pending_count(
    const std::deque<mesh_remote_request_t> *request_queue)
{
    return request_queue ? request_queue->size() : 0U;
}

static std::string daemon_remote_request_status_response(
    const std::deque<mesh_remote_request_t> *request_queue)
{
    std::string out;
    char line[256];
    uint64_t now_us = monotonic_us();

    snprintf(line, sizeof(line), "OK request_status pending=%u history=%u\n",
             (unsigned)mesh_remote_status_pending_count(request_queue),
             (unsigned)mesh_remote_status_count);
    out += line;
    for(size_t n = 0; n < mesh_remote_status_count; n++) {
        size_t idx = (mesh_remote_status_next +
                      MESHTASTIC_REMOTE_STATUS_HISTORY_MAX -
                      mesh_remote_status_count + n) %
                     MESHTASTIC_REMOTE_STATUS_HISTORY_MAX;
        const mesh_remote_request_status_t &entry =
            mesh_remote_status_history[idx];
        uint64_t age_ms = 0ULL;

        if(entry.updated_us != 0ULL && now_us >= entry.updated_us) {
            age_ms = (now_us - entry.updated_us) / 1000ULL;
        }
        uint64_t latency_ms = 0ULL;

        if(entry.replied_us != 0ULL && entry.queued_us != 0ULL &&
           entry.replied_us >= entry.queued_us) {
            latency_ms = (entry.replied_us - entry.queued_us) / 1000ULL;
        }
        snprintf(line, sizeof(line),
                 "REQ id=%u target=0x%08x type=%s state=%s age_ms=%llu "
                 "airtime_ms=%u packet=0x%08x latency_ms=%llu detail=%s\n",
                 entry.request_id, entry.to_node,
                 mesh_remote_request_name(entry.type), entry.state,
                 (unsigned long long)age_ms, entry.airtime_ms,
                 entry.mesh_packet_id, (unsigned long long)latency_ms,
                 entry.detail[0] ? entry.detail : "-");
        out += line;
    }
    return out;
}

static bool mesh_select_local_position(const probe_options_t &opts,
                                       mesh_position_info_t *position,
                                       const char **source)
{
    if(!position) {
        return false;
    }
    if(fixed_position_from_opts(opts, position)) {
        if(source) {
            *source = "fixed";
        }
        return true;
    }
    if(mesh_gnss.present && mesh_gnss.has_fix) {
        *position = mesh_gnss.position;
        if(source) {
            *source = "gnss";
        }
        return true;
    }
    if(source) {
        *source = "unavailable";
    }
    return false;
}

static bool mesh_request_wants_environment_telemetry(
    const std::vector<uint8_t> &payload)
{
    size_t pos = 0;

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
            return field == 3U;
        }
        if(wire == 0U) {
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
    return false;
}

static bool build_mesh_direct_data_frame(const probe_options_t &opts,
                                         uint32_t to_node,
                                         uint8_t channel,
                                         uint32_t portnum,
                                         const std::vector<uint8_t> &payload,
                                         bool want_response,
                                         uint32_t data_dest,
                                         uint32_t request_id,
                                         uint32_t reply_id,
                                         bool request_ack,
                                         const char *summary_kind,
                                         const std::string *psk_override,
                                         tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> data_proto;
    mesh_header_t header;
    uint32_t packet_id;
    char summary[260];
    const std::string &psk = psk_override ? *psk_override : opts.psk;

    if(!frame || to_node == 0U || meshtastic_node_is_broadcast(to_node) ||
       !parse_psk(psk, &key) ||
       !encode_data_proto(portnum, payload, request_id, reply_id, &data_proto,
                          want_response, data_dest, 0U)) {
        return false;
    }
    packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
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
    header.to = to_node;
    header.from = opts.from_node;
    header.id = packet_id;
    header.flags = (opts.hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                   ((opts.hop_limit << MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT) &
                    MESHTASTIC_PACKET_FLAGS_HOP_START_MASK);
    if(request_ack) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    header.channel = channel;
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), data_proto.begin(),
                        data_proto.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = false;
    frame->phoneapi_origin = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh %s id=0x%08x from=0x%08x to=0x%08x ch=0x%02x port=%u payload=%u want_response=%s request=0x%08x reply=0x%08x ack=%s",
             summary_kind && summary_kind[0] ? summary_kind : "direct",
             header.id, header.from, header.to, header.channel, portnum,
             (unsigned)payload.size(), want_response ? "yes" : "no",
             request_id, reply_id, frame->want_ack ? "yes" : "no");
    frame->summary = summary;
    return true;
}

static bool mesh_portnum_uses_pki_direct(uint32_t portnum)
{
    return portnum != MESHTASTIC_NODEINFO_APP &&
           portnum != MESHTASTIC_POSITION_APP &&
           portnum != MESHTASTIC_ROUTING_APP &&
           portnum != MESHTASTIC_TRACEROUTE_APP;
}

static bool build_mesh_pki_direct_data_frame(const probe_options_t &opts,
                                             uint32_t to_node,
                                             uint32_t portnum,
                                             const std::vector<uint8_t> &payload,
                                             uint32_t request_id,
                                             uint32_t reply_id,
                                             bool want_response,
                                             uint32_t data_dest,
                                             bool request_ack,
                                             bool phoneapi_origin,
                                             const char *summary_kind,
                                             tx_frame_t *frame)
{
    uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN];
    std::vector<uint8_t> data_proto;
    std::vector<uint8_t> encrypted_payload;
    mesh_header_t header;
    uint32_t packet_id;
    char summary[260];

    if(!frame || to_node == 0U || meshtastic_node_is_broadcast(to_node) ||
       !mesh_pki_identity.ready ||
       !mesh_node_copy_public_key(to_node, remote_public) ||
       !mesh_portnum_uses_pki_direct(portnum) ||
       !encode_data_proto(portnum, payload, request_id, reply_id,
                          &data_proto, want_response, data_dest, 0U)) {
        return false;
    }
    packet_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
    if(packet_id == 0U) {
        packet_id = 1U;
    }
    if(!mesh_pki_encrypt_payload(remote_public, opts.from_node, packet_id,
                                 data_proto, &encrypted_payload)) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.to = to_node;
    header.from = opts.from_node;
    header.id = packet_id;
    header.flags = (opts.hop_limit & MESHTASTIC_PACKET_FLAGS_HOP_LIMIT_MASK) |
                   ((opts.hop_limit << MESHTASTIC_PACKET_FLAGS_HOP_START_SHIFT) &
                    MESHTASTIC_PACKET_FLAGS_HOP_START_MASK);
    if(request_ack) {
        header.flags |= MESHTASTIC_PACKET_FLAGS_WANT_ACK_MASK;
    }
    header.channel = 0U;
    header.next_hop = 0;
    header.relay_node = (uint8_t)(opts.from_node & 0xffU);

    frame->bytes.clear();
    append_mesh_header(&frame->bytes, header);
    frame->bytes.insert(frame->bytes.end(), encrypted_payload.begin(),
                        encrypted_payload.end());
    frame->rebroadcast = false;
    frame->want_ack = mesh_header_want_ack(header);
    frame->routing_ack = false;
    frame->phoneapi_origin = phoneapi_origin;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = 0;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh %s pki id=0x%08x from=0x%08x to=0x%08x port=%u payload=%u want_response=%s request=0x%08x reply=0x%08x ack=%s",
             summary_kind && summary_kind[0] ? summary_kind : "direct",
             header.id, header.from, header.to, portnum,
             (unsigned)payload.size(), want_response ? "yes" : "no",
             request_id, reply_id, frame->want_ack ? "yes" : "no");
    frame->summary = summary;
    return true;
}

static bool build_mesh_remote_request_frame(const probe_options_t &opts,
                                            const mesh_remote_request_t &req,
                                            tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> payload;
    uint32_t portnum = MESHTASTIC_NODEINFO_APP;
    uint32_t data_dest = 0U;
    bool request_ack = false;

    if(req.to_node == 0U || meshtastic_node_is_broadcast(req.to_node) ||
       !parse_psk(opts.psk, &key)) {
        return false;
    }
    switch(req.type) {
    case MESH_REMOTE_REQ_NODEINFO:
        portnum = MESHTASTIC_NODEINFO_APP;
        if(!encode_user_proto(opts, &payload)) {
            return false;
        }
        break;
    case MESH_REMOTE_REQ_POSITION:
        portnum = MESHTASTIC_POSITION_APP;
        data_dest = req.to_node;
        payload.clear();
        break;
    case MESH_REMOTE_REQ_TELEMETRY_DEVICE:
        portnum = MESHTASTIC_TELEMETRY_APP;
        data_dest = req.to_node;
        if(!encode_telemetry_request_proto(false, &payload)) {
            return false;
        }
        break;
    case MESH_REMOTE_REQ_TELEMETRY_ENVIRONMENT:
        portnum = MESHTASTIC_TELEMETRY_APP;
        data_dest = req.to_node;
        if(!encode_telemetry_request_proto(true, &payload)) {
            return false;
        }
        break;
    case MESH_REMOTE_REQ_TRACEROUTE:
        portnum = MESHTASTIC_TRACEROUTE_APP;
        data_dest = req.to_node;
        break;
    case MESH_REMOTE_REQ_NEIGHBORINFO:
        portnum = MESHTASTIC_NEIGHBORINFO_APP;
        data_dest = req.to_node;
        break;
    default:
        return false;
    }
    return build_mesh_direct_data_frame(opts, req.to_node,
                                        mesh_channel_hash(
                                            effective_mesh_channel_name(opts),
                                            key),
                                        portnum, payload, true, data_dest, 0U,
                                        0U, request_ack,
                                        mesh_remote_request_name(req.type),
                                        nullptr,
                                        frame);
}

static bool build_mesh_want_response_frame(const probe_options_t &opts,
                                           const mesh_header_t &rx_header,
                                           const mesh_data_proto_t &request,
                                           float rx_snr,
                                           const std::string *psk_override,
                                           bool pki_response,
                                           tx_frame_t *frame)
{
    std::vector<uint8_t> payload;
    uint32_t portnum = request.portnum;
    bool environment;
    mesh_position_info_t position;
    mesh_telemetry_info_t telemetry;

    if(!frame || rx_header.from == 0U ||
       rx_header.from == opts.from_node ||
       meshtastic_node_is_broadcast(rx_header.from)) {
        return false;
    }
    if(request.portnum == MESHTASTIC_NODEINFO_APP) {
        if(!encode_user_proto(opts, &payload)) {
            return false;
        }
    } else if(request.portnum == MESHTASTIC_POSITION_APP) {
        const char *source = "unavailable";

        if(!mesh_select_local_position(opts, &position, &source)) {
            daemon_event("WantResponse position skipped to=0x%08x gps=%s/%s",
                         rx_header.from, mesh_gnss.modem_state,
                         mesh_gnss.gps_state);
            return false;
        }
        if(!encode_position_proto(position, opts.position_interval_sec,
                                  &payload)) {
            return false;
        }
        daemon_event("WantResponse position using %s to=0x%08x lat=%.7f lon=%.7f",
                     source, rx_header.from,
                     position.latitude_i * 1e-7,
                     position.longitude_i * 1e-7);
    } else if(request.portnum == MESHTASTIC_TELEMETRY_APP) {
        environment = mesh_request_wants_environment_telemetry(request.payload);
        if(environment) {
            if(!mesh_collect_environment_telemetry(&telemetry)) {
                daemon_event("WantResponse env telemetry skipped to=0x%08x",
                             rx_header.from);
                return false;
            }
        } else if(!mesh_collect_device_telemetry(&telemetry)) {
            daemon_event("WantResponse device telemetry skipped to=0x%08x",
                         rx_header.from);
            return false;
        }
        if(!encode_telemetry_proto(telemetry, environment, &payload)) {
            return false;
        }
    } else if(request.portnum == MESHTASTIC_TRACEROUTE_APP) {
        if(!encode_route_discovery_response_proto(opts, rx_header,
                                                  rx_snr,
                                                  &payload)) {
            return false;
        }
    } else if(request.portnum == MESHTASTIC_NEIGHBORINFO_APP) {
        if(!encode_neighbor_info_proto(opts, &payload)) {
            return false;
        }
    } else {
        return false;
    }
    if(pki_response &&
       build_mesh_pki_direct_data_frame(opts, rx_header.from, portnum,
                                        payload, rx_header.id, 0U, false,
                                        0U, false, false,
                                        "want-response", frame)) {
        return true;
    }
    return build_mesh_direct_data_frame(opts, rx_header.from,
                                        rx_header.channel, portnum, payload,
                                        false, 0U, rx_header.id, 0U,
                                        false,
                                        "want-response", psk_override, frame);
}

static bool build_mesh_ack_frame(const probe_options_t &opts,
                                 const mesh_header_t &rx_header,
                                 uint32_t error_reason,
                                 bool ack_wants_ack,
                                 const std::string *psk_override,
                                 tx_frame_t *frame)
{
    std::vector<uint8_t> key;
    std::vector<uint8_t> routing_proto;
    std::vector<uint8_t> data_proto;
    mesh_header_t header;
    uint32_t packet_id;
    uint8_t ack_hop;
    char summary[220];
    const std::string &psk = psk_override ? *psk_override : opts.psk;

    if(!frame || !parse_psk(psk, &key)) {
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
    frame->phoneapi_origin = false;
    frame->to_node = header.to;
    frame->from_node = header.from;
    frame->packet_id = header.id;
    frame->ack_request_id = rx_header.id;
    frame->channel = header.channel;
    snprintf(summary, sizeof(summary),
             "mesh ack id=0x%08x req=0x%08x from=0x%08x to=0x%08x ch=0x%02x hop=%u err=%u(%s) ack=%s",
             header.id, rx_header.id, header.from, header.to, header.channel,
             ack_hop, error_reason, mesh_routing_error_name(error_reason),
             frame->want_ack ? "on" : "off");
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
                           const std::string &message,
                           uint32_t channel_index,
                           tx_frame_t *frame)
{
    if(opts.mesh_mode) {
        return build_mesh_frame(opts, message, channel_index, frame);
    }
    if(!frame) {
        return false;
    }
    *frame = build_raw_frame(opts, message);
    return true;
}

static bool build_tx_data_frame(const probe_options_t &opts, uint32_t portnum,
                                const std::vector<uint8_t> &payload,
                                uint32_t channel_index,
                                const char *summary_kind, tx_frame_t *frame)
{
    if(!opts.mesh_mode || !frame) {
        return false;
    }
    return build_mesh_data_frame(opts, portnum, payload, channel_index,
                                 summary_kind, frame);
}

typedef struct {
    bool active = false;
    uint32_t from_node = 0;
    uint32_t stream_id = 0;
    uint16_t total = 0;
    uint64_t first_us = 0;
    uint64_t last_us = 0;
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> received;
} mesh_voice_rx_stream_t;

typedef struct {
    bool active = false;
    uint32_t from_node = 0;
    uint32_t stream_id = 0;
    uint32_t payload_crc = 0;
    uint16_t total = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint64_t updated_us = 0;
    std::vector<uint8_t> payload;
} mesh_photo_tx_cache_entry_t;

typedef struct {
    bool enabled = false;
    uint8_t rate_percent = 0;
    uint16_t seq[MESHTASTIC_FLRC_PHOTO_DEBUG_DROP_MAX_SEQ];
    uint16_t seq_count = 0;
    uint64_t hit_count = 0;
} mesh_photo_debug_drop_t;

typedef struct {
    uint8_t photo_data_repeat = MESHTASTIC_FLRC_PHOTO_DATA_REPEAT;
    uint8_t photo_repair_rounds = MESHTASTIC_FLRC_PHOTO_REPAIR_MAX_ROUNDS;
    uint8_t photo_repair_repeat =
        MESHTASTIC_FLRC_PHOTO_REPAIR_DATA_REPEAT;
    uint32_t photo_repair_window_ms =
        (uint32_t)MESHTASTIC_FLRC_PHOTO_REPAIR_WINDOW_MS;
    uint32_t photo_tx_cache_ttl_sec =
        (uint32_t)MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_TTL_SEC;
} mesh_media_config_t;

static mesh_voice_rx_stream_t mesh_voice_rx_streams[MESHTASTIC_VOICE_RX_STREAMS];
static mesh_photo_tx_cache_entry_t
    mesh_photo_tx_cache[MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_SIZE];
static mesh_photo_debug_drop_t mesh_photo_debug_drop;
static mesh_media_config_t mesh_media_cfg;
static uint64_t mesh_voice_tx_stream_count;
static uint64_t mesh_voice_tx_chunk_count;
static uint64_t mesh_voice_rx_chunk_count;
static uint64_t mesh_voice_rx_complete_count;
static uint64_t mesh_voice_rx_decode_fail_count;
static uint64_t mesh_photo_tx_stream_count;
static uint64_t mesh_photo_tx_chunk_count;
static uint64_t mesh_photo_rx_chunk_count;
static uint64_t mesh_photo_rx_complete_count;
static uint64_t mesh_photo_rx_decode_fail_count;
static uint64_t mesh_photo_repair_req_tx_count;
static uint64_t mesh_photo_repair_req_rx_count;
static uint64_t mesh_photo_repair_tx_chunk_count;
static uint64_t mesh_photo_repair_rx_chunk_count;
static uint64_t mesh_photo_repair_complete_count;
static uint64_t mesh_photo_repair_fail_count;

static void mesh_voice_rx_stream_reset(mesh_voice_rx_stream_t *stream)
{
    if(!stream) {
        return;
    }
    stream->active = false;
    stream->from_node = 0;
    stream->stream_id = 0;
    stream->total = 0;
    stream->first_us = 0;
    stream->last_us = 0;
    stream->chunks.clear();
    stream->received.clear();
}

static void mesh_photo_tx_cache_expire(uint64_t now_us)
{
    uint64_t ttl_us =
        (uint64_t)mesh_media_cfg.photo_tx_cache_ttl_sec * 1000000ULL;

    for(size_t i = 0; i < MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_SIZE; i++) {
        mesh_photo_tx_cache_entry_t *entry = &mesh_photo_tx_cache[i];

        if(entry->active &&
           now_us - entry->updated_us > ttl_us) {
            entry->active = false;
            entry->payload.clear();
        }
    }
}

static void mesh_photo_tx_cache_store(const tx_frame_t &frame)
{
    uint64_t now_us = monotonic_us();
    mesh_photo_tx_cache_entry_t *slot = nullptr;

    if(!frame.flrc_photo_after_tx || frame.flrc_photo_payload.empty() ||
       frame.flrc_photo_stream_id == 0U) {
        return;
    }
    mesh_photo_tx_cache_expire(now_us);
    for(size_t i = 0; i < MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_SIZE; i++) {
        if(mesh_photo_tx_cache[i].active &&
           mesh_photo_tx_cache[i].stream_id == frame.flrc_photo_stream_id) {
            slot = &mesh_photo_tx_cache[i];
            break;
        }
        if(!mesh_photo_tx_cache[i].active && !slot) {
            slot = &mesh_photo_tx_cache[i];
        }
    }
    if(!slot) {
        slot = &mesh_photo_tx_cache[0];
        for(size_t i = 1; i < MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_SIZE; i++) {
            if(mesh_photo_tx_cache[i].updated_us < slot->updated_us) {
                slot = &mesh_photo_tx_cache[i];
            }
        }
    }

    slot->active = true;
    slot->from_node = frame.from_node;
    slot->stream_id = frame.flrc_photo_stream_id;
    slot->payload_crc = frame.flrc_photo_payload_crc;
    slot->total = frame.flrc_photo_total_packets;
    slot->width = frame.flrc_photo_width;
    slot->height = frame.flrc_photo_height;
    slot->updated_us = now_us;
    slot->payload = frame.flrc_photo_payload;
    daemon_event("FLRC photo TX cache store stream=0x%08x packets=%u bytes=%u",
                 slot->stream_id, slot->total, (unsigned)slot->payload.size());
}

static mesh_photo_tx_cache_entry_t *mesh_photo_tx_cache_find(
    uint32_t stream_id, uint32_t stream_crc, uint16_t total,
    uint32_t total_size)
{
    uint64_t now_us = monotonic_us();

    mesh_photo_tx_cache_expire(now_us);
    for(size_t i = 0; i < MESHTASTIC_FLRC_PHOTO_REPAIR_TX_CACHE_SIZE; i++) {
        mesh_photo_tx_cache_entry_t *entry = &mesh_photo_tx_cache[i];

        if(!entry->active || entry->stream_id != stream_id) {
            continue;
        }
        if(entry->payload_crc != stream_crc || entry->total != total ||
           entry->payload.size() != total_size) {
            daemon_event("FLRC photo TX cache mismatch stream=0x%08x want_crc=0x%08x have_crc=0x%08x want_packets=%u have_packets=%u want_bytes=%u have_bytes=%u",
                         stream_id, stream_crc, entry->payload_crc, total,
                         entry->total, total_size,
                         (unsigned)entry->payload.size());
            return nullptr;
        }
        entry->updated_us = now_us;
        return entry;
    }
    return nullptr;
}

static bool mesh_voice_payload_is_k230(const std::vector<uint8_t> &payload)
{
    return payload.size() >= 16U &&
           memcmp(payload.data(), MESHTASTIC_VOICE_MAGIC, 4U) == 0;
}

static bool mesh_voice_payload_header(const std::vector<uint8_t> &payload,
                                      uint32_t *stream_id, uint16_t *seq,
                                      uint16_t *total)
{
    if(!mesh_voice_payload_is_k230(payload) || !stream_id || !seq || !total ||
       payload[12] != (MESHTASTIC_VOICE_SAMPLE_RATE / 1000U) ||
       payload[13] != MESHTASTIC_VOICE_FRAME_MS ||
       payload[14] != MESHTASTIC_VOICE_CODEC_OPUS) {
        return false;
    }
    *stream_id = get_le32(payload.data() + 4U);
    *seq = get_le16(payload.data() + 8U);
    *total = get_le16(payload.data() + 10U);
    if(*total == 0U || *total > MESHTASTIC_DAEMON_SEND_QUEUE_MAX ||
       *seq >= *total) {
        return false;
    }
    return true;
}

static bool mesh_voice_chunk_payload_valid(const std::vector<uint8_t> &payload)
{
    size_t pos = 16U;
    uint8_t frame_count;

    if(!mesh_voice_payload_is_k230(payload)) {
        return false;
    }
    frame_count = payload[15];
    for(uint8_t i = 0U; i < frame_count; i++) {
        uint8_t frame_len;
        if(pos >= payload.size()) {
            return false;
        }
        frame_len = payload[pos++];
        if(frame_len == 0U || pos + frame_len > payload.size()) {
            return false;
        }
        pos += frame_len;
    }
    return pos == payload.size();
}

static bool mesh_voice_payload_is_codec2(const std::vector<uint8_t> &payload)
{
    return payload.size() >= MESHTASTIC_VOICE_CODEC2_HEADER_LEN &&
           payload[0] == 0xc0U && payload[1] == 0xdeU &&
           payload[2] == 0xc2U;
}

static void mesh_voice_set_error(char *errbuf, size_t errbuf_len,
                                 const char *fmt, ...)
{
    va_list ap;

    if(!errbuf || errbuf_len == 0U || !fmt) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(errbuf, errbuf_len, fmt, ap);
    va_end(ap);
}

static mesh_voice_rx_stream_t *mesh_voice_rx_find_stream(uint32_t from_node,
                                                         uint32_t stream_id,
                                                         uint16_t total)
{
    uint64_t now = monotonic_us();
    mesh_voice_rx_stream_t *oldest = &mesh_voice_rx_streams[0];

    for(size_t i = 0; i < MESHTASTIC_VOICE_RX_STREAMS; i++) {
        mesh_voice_rx_stream_t *stream = &mesh_voice_rx_streams[i];
        if(stream->active && now - stream->last_us > 120000000ULL) {
            mesh_voice_rx_stream_reset(stream);
        }
        if(stream->active && stream->from_node == from_node &&
           stream->stream_id == stream_id) {
            return stream;
        }
        if(!stream->active) {
            stream->active = true;
            stream->from_node = from_node;
            stream->stream_id = stream_id;
            stream->total = total;
            stream->first_us = now;
            stream->last_us = now;
            stream->chunks.assign(total, std::vector<uint8_t>());
            stream->received.assign(total, 0U);
            return stream;
        }
        if(stream->last_us < oldest->last_us) {
            oldest = stream;
        }
    }

    mesh_voice_rx_stream_reset(oldest);
    oldest->active = true;
    oldest->from_node = from_node;
    oldest->stream_id = stream_id;
    oldest->total = total;
    oldest->first_us = now;
    oldest->last_us = now;
    oldest->chunks.assign(total, std::vector<uint8_t>());
    oldest->received.assign(total, 0U);
    return oldest;
}

static bool mesh_voice_rx_complete(const mesh_voice_rx_stream_t *stream)
{
    if(!stream || !stream->active || stream->total == 0U ||
       stream->received.size() < stream->total) {
        return false;
    }
    for(uint16_t i = 0U; i < stream->total; i++) {
        if(!stream->received[i]) {
            return false;
        }
    }
    return true;
}

static bool mesh_voice_decode_stream_to_file(const mesh_voice_rx_stream_t *stream,
                                             const char *path,
                                             unsigned *duration_ms)
{
    int err = OPUS_OK;
    OpusDecoder *decoder;
    std::vector<int16_t> pcm;
    int16_t out[MESHTASTIC_VOICE_FRAME_SAMPLES];
    FILE *fp;

    if(duration_ms) {
        *duration_ms = 0U;
    }
    if(!stream || !path || !stream->active || !mesh_voice_rx_complete(stream)) {
        return false;
    }
    decoder = opus_decoder_create(MESHTASTIC_VOICE_SAMPLE_RATE, 1, &err);
    if(!decoder || err != OPUS_OK) {
        daemon_event("Voice decode failed: opus decoder create rc=%d", err);
        if(decoder) {
            opus_decoder_destroy(decoder);
        }
        return false;
    }
    for(uint16_t seq = 0U; seq < stream->total; seq++) {
        const std::vector<uint8_t> &payload = stream->chunks[seq];
        size_t pos = 16U;
        uint8_t frame_count = payload[15];

        for(uint8_t i = 0U; i < frame_count; i++) {
            uint8_t frame_len = payload[pos++];
            int samples = opus_decode(decoder, payload.data() + pos, frame_len,
                                      out, MESHTASTIC_VOICE_FRAME_SAMPLES, 0);
            pos += frame_len;
            if(samples <= 0) {
                daemon_event("Voice decode chunk failed stream=0x%08x seq=%u frame=%u rc=%d",
                             stream->stream_id, seq, i, samples);
                continue;
            }
            pcm.insert(pcm.end(), out, out + samples);
        }
    }
    opus_decoder_destroy(decoder);
    if(pcm.empty()) {
        return false;
    }
    fp = fopen(path, "wb");
    if(!fp) {
        daemon_event("Voice decode fopen failed path=%s err=%s", path,
                     strerror(errno));
        return false;
    }
    fwrite(pcm.data(), sizeof(int16_t), pcm.size(), fp);
    fclose(fp);
    if(duration_ms) {
        *duration_ms = (unsigned)((uint64_t)pcm.size() * 1000ULL /
                                  MESHTASTIC_VOICE_SAMPLE_RATE);
    }
    return true;
}

static bool mesh_voice_decode_codec2_payload_to_file(
    const std::vector<uint8_t> &payload, const char *path,
    unsigned *duration_ms, char *errbuf, size_t errbuf_len)
{
    CODEC2 *codec;
    uint8_t mode;
    int frame_bytes;
    int frame_samples;
    size_t payload_pos = MESHTASTIC_VOICE_CODEC2_HEADER_LEN;
    std::vector<int16_t> pcm;
    FILE *fp;

    if(duration_ms) {
        *duration_ms = 0U;
    }
    if(!path || !path[0]) {
        mesh_voice_set_error(errbuf, errbuf_len, "invalid codec2 output path");
        return false;
    }
    if(!mesh_voice_payload_is_codec2(payload)) {
        mesh_voice_set_error(errbuf, errbuf_len, "invalid codec2 header");
        return false;
    }

    mode = payload[3];
    codec = codec2_create((int)mode);
    if(!codec) {
        mesh_voice_set_error(errbuf, errbuf_len, "codec2 create mode=%u failed",
                             mode);
        return false;
    }
    codec2_set_lpc_post_filter(codec, 1, 0, 0.8f, 0.2f);
    frame_bytes = (codec2_bits_per_frame(codec) + 7) / 8;
    frame_samples = codec2_samples_per_frame(codec);
    if(frame_bytes <= 0 || frame_samples <= 0) {
        codec2_destroy(codec);
        mesh_voice_set_error(errbuf, errbuf_len,
                             "codec2 bad frame mode=%u bytes=%d samples=%d",
                             mode, frame_bytes, frame_samples);
        return false;
    }

    while(payload_pos + (size_t)frame_bytes <= payload.size()) {
        size_t old_size = pcm.size();
        pcm.resize(old_size + (size_t)frame_samples);
        codec2_decode(codec, pcm.data() + old_size, payload.data() + payload_pos);
        payload_pos += (size_t)frame_bytes;
    }
    codec2_destroy(codec);
    if(pcm.empty()) {
        mesh_voice_set_error(errbuf, errbuf_len,
                             "codec2 payload has no complete frame mode=%u len=%u",
                             mode, (unsigned)payload.size());
        return false;
    }
    if(payload_pos != payload.size()) {
        daemon_event("Codec2 voice ignored trailing bytes mode=%u trailing=%u",
                     mode, (unsigned)(payload.size() - payload_pos));
    }

    fp = fopen(path, "wb");
    if(!fp) {
        mesh_voice_set_error(errbuf, errbuf_len, "codec2 fopen failed: %s",
                             strerror(errno));
        return false;
    }
    fwrite(pcm.data(), sizeof(int16_t), pcm.size(), fp);
    fclose(fp);
    if(duration_ms) {
        *duration_ms = (unsigned)((uint64_t)pcm.size() * 1000ULL /
                                  MESHTASTIC_VOICE_SAMPLE_RATE);
    }
    return true;
}

static void mesh_voice_play_file_async(const char *path)
{
    char command[320];

    if(!path || !path[0]) {
        return;
    }
    snprintf(command, sizeof(command),
             "aplay -q -f S16_LE -c 1 -r %u '%s' >/dev/null 2>&1 &",
             MESHTASTIC_VOICE_SAMPLE_RATE, path);
    (void)system(command);
}

static bool mesh_voice_copy_pcm_cache(const std::string &src,
                                      uint32_t stream_id,
                                      std::string *dst)
{
    char path[96];
    FILE *in;
    FILE *out;
    uint8_t buf[4096];
    size_t n;
    bool ok = true;

    if(dst) {
        dst->clear();
    }
    if(src.empty() || stream_id == 0U || !dst) {
        return false;
    }
    snprintf(path, sizeof(path), "/tmp/k230_mesh_voice_tx_%08x.raw",
             stream_id);
    in = fopen(src.c_str(), "rb");
    if(!in) {
        daemon_event("Voice TX cache open failed src=%s err=%s",
                     src.c_str(), strerror(errno));
        return false;
    }
    out = fopen(path, "wb");
    if(!out) {
        daemon_event("Voice TX cache create failed path=%s err=%s",
                     path, strerror(errno));
        fclose(in);
        return false;
    }
    while((n = fread(buf, 1U, sizeof(buf), in)) > 0U) {
        if(fwrite(buf, 1U, n, out) != n) {
            daemon_event("Voice TX cache write failed path=%s err=%s",
                         path, strerror(errno));
            ok = false;
            break;
        }
    }
    if(ferror(in)) {
        daemon_event("Voice TX cache read failed src=%s err=%s",
                     src.c_str(), strerror(errno));
        ok = false;
    }
    fclose(out);
    fclose(in);
    if(!ok) {
        unlink(path);
        return false;
    }
    *dst = path;
    return true;
}

static bool mesh_voice_handle_rx(const probe_options_t &opts,
                                 const mesh_header_t &header,
                                 const std::vector<uint8_t> &payload,
                                 float rssi, float snr, bool duplicate,
                                 bool secure_match)
{
    uint32_t stream_id;
    uint16_t seq;
    uint16_t total;
    mesh_voice_rx_stream_t *stream;

    if(!mesh_voice_payload_header(payload, &stream_id, &seq, &total) ||
       !mesh_voice_chunk_payload_valid(payload)) {
        return false;
    }
    daemon_event("RX voice chunk from=0x%08x id=0x%08x stream=0x%08x seq=%u/%u len=%u rssi=%.1f snr=%.1f%s",
                 header.from, header.id, stream_id, seq + 1U, total,
                 (unsigned)payload.size(), rssi, snr,
                 duplicate ? " duplicate" : "");
    if(duplicate || !secure_match || header.from == opts.from_node) {
        return true;
    }
    mesh_voice_rx_chunk_count++;
    stream = mesh_voice_rx_find_stream(header.from, stream_id, total);
    if(!stream || stream->total != total || seq >= stream->total) {
        return true;
    }
    stream->last_us = monotonic_us();
    if(!stream->received[seq]) {
        stream->chunks[seq] = payload;
        stream->received[seq] = 1U;
    }
    if(mesh_voice_rx_complete(stream)) {
        char path[128];
        unsigned duration_ms = 0U;
        snprintf(path, sizeof(path), "/tmp/k230_mesh_voice_rx_%08x_%08x.raw",
                 header.from, stream_id);
        if(mesh_voice_decode_stream_to_file(stream, path, &duration_ms)) {
            mesh_voice_rx_complete_count++;
            if(rssi > -200.0f && rssi < 20.0f) {
                daemon_chat("RX 0x%08x voice %.1fs chunks=%u rssi=%ddBm file=%s",
                            header.from, (double)duration_ms / 1000.0,
                            stream->total, (int)roundf(rssi), path);
            } else {
                daemon_chat("RX 0x%08x voice %.1fs chunks=%u rssi=-- file=%s",
                            header.from, (double)duration_ms / 1000.0,
                            stream->total, path);
            }
        } else {
            mesh_voice_rx_decode_fail_count++;
            daemon_chat("RX 0x%08x voice decode failed chunks=%u",
                        header.from, stream->total);
        }
        mesh_voice_rx_stream_reset(stream);
    }
    return true;
}

static bool mesh_voice_encode_pcm_file(const char *path, uint32_t stream_id,
                                       std::vector<std::vector<uint8_t>> *chunks,
                                       char *errbuf, size_t errbuf_len)
{
    FILE *fp;
    std::vector<int16_t> pcm;
    uint8_t encoded[96];
    int err = OPUS_OK;
    OpusEncoder *encoder;
    std::vector<uint8_t> chunk;
    size_t read_samples;

    if(errbuf && errbuf_len > 0U) {
        errbuf[0] = '\0';
    }
    if(!path || !chunks) {
        snprintf(errbuf, errbuf_len, "invalid voice argument");
        return false;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        snprintf(errbuf, errbuf_len, "open failed: %s", strerror(errno));
        return false;
    }
    pcm.resize(MESHTASTIC_VOICE_MAX_PCM_BYTES / sizeof(int16_t));
    read_samples = fread(pcm.data(), sizeof(int16_t), pcm.size(), fp);
    fclose(fp);
    pcm.resize(read_samples);
    if(pcm.size() < MESHTASTIC_VOICE_FRAME_SAMPLES) {
        snprintf(errbuf, errbuf_len, "voice sample too short");
        return false;
    }
    while(pcm.size() % MESHTASTIC_VOICE_FRAME_SAMPLES) {
        pcm.push_back(0);
    }

    encoder = opus_encoder_create(MESHTASTIC_VOICE_SAMPLE_RATE, 1,
                                  OPUS_APPLICATION_VOIP, &err);
    if(!encoder || err != OPUS_OK) {
        snprintf(errbuf, errbuf_len, "opus encoder create rc=%d", err);
        if(encoder) {
            opus_encoder_destroy(encoder);
        }
        return false;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(MESHTASTIC_VOICE_BITRATE_BPS));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    chunks->clear();
    for(size_t pos = 0; pos < pcm.size();
        pos += MESHTASTIC_VOICE_FRAME_SAMPLES) {
        int n = opus_encode(encoder, pcm.data() + pos,
                            MESHTASTIC_VOICE_FRAME_SAMPLES, encoded,
                            sizeof(encoded));
        if(n <= 0 || n > 255) {
            snprintf(errbuf, errbuf_len, "opus encode rc=%d", n);
            opus_encoder_destroy(encoder);
            return false;
        }
        if(chunk.empty()) {
            chunk.assign(16U, 0U);
            memcpy(chunk.data(), MESHTASTIC_VOICE_MAGIC, 4U);
            put_le32(chunk.data() + 4U, stream_id);
            chunk[12] = (uint8_t)(MESHTASTIC_VOICE_SAMPLE_RATE / 1000U);
            chunk[13] = (uint8_t)MESHTASTIC_VOICE_FRAME_MS;
            chunk[14] = MESHTASTIC_VOICE_CODEC_OPUS;
            chunk[15] = 0U;
        }
        if(chunk[15] > 0U &&
           chunk.size() + 1U + (size_t)n > MESHTASTIC_VOICE_CHUNK_TARGET_BYTES) {
            chunks->push_back(chunk);
            chunk.clear();
            chunk.assign(16U, 0U);
            memcpy(chunk.data(), MESHTASTIC_VOICE_MAGIC, 4U);
            put_le32(chunk.data() + 4U, stream_id);
            chunk[12] = (uint8_t)(MESHTASTIC_VOICE_SAMPLE_RATE / 1000U);
            chunk[13] = (uint8_t)MESHTASTIC_VOICE_FRAME_MS;
            chunk[14] = MESHTASTIC_VOICE_CODEC_OPUS;
            chunk[15] = 0U;
        }
        chunk.push_back((uint8_t)n);
        chunk.insert(chunk.end(), encoded, encoded + n);
        chunk[15]++;
    }
    opus_encoder_destroy(encoder);
    if(!chunk.empty()) {
        chunks->push_back(chunk);
    }
    if(chunks->empty() || chunks->size() > MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
        snprintf(errbuf, errbuf_len, "voice chunk count %u exceeds queue",
                 (unsigned)chunks->size());
        chunks->clear();
        return false;
    }
    for(size_t i = 0; i < chunks->size(); i++) {
        put_le16((*chunks)[i].data() + 8U, (uint16_t)i);
        put_le16((*chunks)[i].data() + 10U, (uint16_t)chunks->size());
    }
    return true;
}

static bool mesh_voice_encode_codec2_pcm_file(
    const char *path, std::vector<std::vector<uint8_t>> *chunks,
    unsigned *duration_ms, char *errbuf, size_t errbuf_len)
{
    FILE *fp;
    std::vector<int16_t> pcm;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> chunk;
    CODEC2 *codec;
    int frame_bytes;
    int frame_samples;
    size_t read_samples;

    if(duration_ms) {
        *duration_ms = 0U;
    }
    if(errbuf && errbuf_len > 0U) {
        errbuf[0] = '\0';
    }
    if(!path || !chunks) {
        mesh_voice_set_error(errbuf, errbuf_len, "invalid codec2 voice argument");
        return false;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        mesh_voice_set_error(errbuf, errbuf_len, "codec2 open failed: %s",
                             strerror(errno));
        return false;
    }
    pcm.resize(MESHTASTIC_VOICE_MAX_PCM_BYTES / sizeof(int16_t));
    read_samples = fread(pcm.data(), sizeof(int16_t), pcm.size(), fp);
    fclose(fp);
    pcm.resize(read_samples);
    if(duration_ms) {
        *duration_ms = (unsigned)((uint64_t)read_samples * 1000ULL /
                                  MESHTASTIC_VOICE_SAMPLE_RATE);
    }
    if(pcm.empty()) {
        mesh_voice_set_error(errbuf, errbuf_len, "codec2 voice sample empty");
        return false;
    }

    codec = codec2_create(MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE);
    if(!codec) {
        mesh_voice_set_error(errbuf, errbuf_len, "codec2 encoder create failed");
        return false;
    }
    codec2_set_lpc_post_filter(codec, 1, 0, 0.8f, 0.2f);
    frame_bytes = (codec2_bits_per_frame(codec) + 7) / 8;
    frame_samples = codec2_samples_per_frame(codec);
    if(frame_bytes <= 0 || frame_samples <= 0 ||
       (size_t)frame_bytes >
       (MESHTASTIC_DATA_PAYLOAD_LEN - MESHTASTIC_VOICE_CODEC2_HEADER_LEN)) {
        codec2_destroy(codec);
        mesh_voice_set_error(errbuf, errbuf_len,
                             "codec2 bad frame mode=%u bytes=%d samples=%d",
                             MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE,
                             frame_bytes, frame_samples);
        return false;
    }
    while(pcm.size() % (size_t)frame_samples) {
        pcm.push_back(0);
    }

    chunks->clear();
    encoded.resize((size_t)frame_bytes);
    for(size_t pos = 0; pos < pcm.size(); pos += (size_t)frame_samples) {
        if(chunk.empty()) {
            chunk.push_back(0xc0U);
            chunk.push_back(0xdeU);
            chunk.push_back(0xc2U);
            chunk.push_back(MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE);
        }
        if(chunk.size() + (size_t)frame_bytes > MESHTASTIC_DATA_PAYLOAD_LEN) {
            chunks->push_back(chunk);
            chunk.clear();
            chunk.push_back(0xc0U);
            chunk.push_back(0xdeU);
            chunk.push_back(0xc2U);
            chunk.push_back(MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE);
        }
        codec2_encode(codec, encoded.data(), pcm.data() + pos);
        chunk.insert(chunk.end(), encoded.begin(), encoded.end());
    }
    codec2_destroy(codec);
    if(!chunk.empty()) {
        chunks->push_back(chunk);
    }
    if(chunks->empty() || chunks->size() > MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
        mesh_voice_set_error(errbuf, errbuf_len,
                             "codec2 voice packet count %u exceeds queue",
                             (unsigned)chunks->size());
        chunks->clear();
        return false;
    }
    return true;
}

static bool mesh_voice_encode_codec2_pcm_file_stream(
    const char *path, std::vector<uint8_t> *payload, unsigned *duration_ms,
    char *errbuf, size_t errbuf_len)
{
    std::vector<std::vector<uint8_t>> chunks;

    if(!payload) {
        mesh_voice_set_error(errbuf, errbuf_len,
                             "invalid codec2 stream argument");
        return false;
    }
    if(!mesh_voice_encode_codec2_pcm_file(path, &chunks, duration_ms, errbuf,
                                          errbuf_len)) {
        payload->clear();
        return false;
    }
    payload->clear();
    for(size_t i = 0; i < chunks.size(); i++) {
        if(!mesh_voice_payload_is_codec2(chunks[i])) {
            mesh_voice_set_error(errbuf, errbuf_len,
                                 "codec2 stream chunk header invalid");
            payload->clear();
            return false;
        }
        if(i == 0U) {
            payload->insert(payload->end(), chunks[i].begin(), chunks[i].end());
        } else {
            payload->insert(payload->end(),
                            chunks[i].begin() +
                                MESHTASTIC_VOICE_CODEC2_HEADER_LEN,
                            chunks[i].end());
        }
    }
    if(payload->empty()) {
        mesh_voice_set_error(errbuf, errbuf_len, "codec2 stream empty");
        return false;
    }
    return true;
}

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
} mesh_jpeg_error_mgr_t;

static void mesh_jpeg_error_exit(j_common_ptr cinfo)
{
    mesh_jpeg_error_mgr_t *err =
        (mesh_jpeg_error_mgr_t *)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

static void mesh_photo_set_error(char *errbuf, size_t errbuf_len,
                                 const char *fmt, ...)
{
    va_list ap;

    if(!errbuf || errbuf_len == 0U) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(errbuf, errbuf_len, fmt, ap);
    va_end(ap);
}

static bool mesh_photo_source_path_allowed(const std::string &path)
{
    const char *prefix = MESHTASTIC_PHOTO_SOURCE_DIR "/";
    size_t prefix_len = strlen(prefix);
    size_t len = path.size();

    if(path.compare(0, prefix_len, prefix) != 0 ||
       path.find('\n') != std::string::npos ||
       path.find('\r') != std::string::npos) {
        return false;
    }
    if(path.find("/../") != std::string::npos ||
       path.find("/./") != std::string::npos) {
        return false;
    }
    return (len > 4U && strcasecmp(path.c_str() + len - 4U, ".ppm") == 0) ||
           (len > 4U && strcasecmp(path.c_str() + len - 4U, ".jpg") == 0) ||
           (len > 5U && strcasecmp(path.c_str() + len - 5U, ".jpeg") == 0);
}

static bool mesh_photo_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if(!path || !suffix) {
        return false;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           strcasecmp(path + path_len - suffix_len, suffix) == 0;
}

static bool mesh_photo_read_ppm_token(FILE *fp, char *token,
                                      size_t token_len)
{
    int c;
    size_t n = 0U;

    if(!fp || !token || token_len == 0U) {
        return false;
    }
    token[0] = '\0';
    while((c = fgetc(fp)) != EOF) {
        if(isspace(c)) {
            continue;
        }
        if(c == '#') {
            while((c = fgetc(fp)) != EOF && c != '\n') {
            }
            continue;
        }
        break;
    }
    if(c == EOF) {
        return false;
    }
    do {
        if(c == '#') {
            while((c = fgetc(fp)) != EOF && c != '\n') {
            }
            break;
        }
        if(isspace(c)) {
            break;
        }
        if(n + 1U < token_len) {
            token[n++] = (char)c;
        }
    } while((c = fgetc(fp)) != EOF);
    token[n] = '\0';
    return n > 0U;
}

static bool mesh_photo_read_ppm_rgb(const char *path,
                                    std::vector<uint8_t> *rgb,
                                    unsigned *width, unsigned *height,
                                    char *errbuf, size_t errbuf_len)
{
    FILE *fp;
    char token[32];
    long data_len;
    unsigned w;
    unsigned h;
    unsigned maxval;
    size_t want;
    size_t got;

    if(!path || !rgb || !width || !height) {
        mesh_photo_set_error(errbuf, errbuf_len, "invalid photo argument");
        return false;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        mesh_photo_set_error(errbuf, errbuf_len, "open failed: %s",
                             strerror(errno));
        return false;
    }
    if(!mesh_photo_read_ppm_token(fp, token, sizeof(token)) ||
       strcmp(token, "P6") != 0 ||
       !mesh_photo_read_ppm_token(fp, token, sizeof(token))) {
        fclose(fp);
        mesh_photo_set_error(errbuf, errbuf_len, "unsupported image format");
        return false;
    }
    w = (unsigned)strtoul(token, nullptr, 10);
    if(!mesh_photo_read_ppm_token(fp, token, sizeof(token))) {
        fclose(fp);
        mesh_photo_set_error(errbuf, errbuf_len, "missing ppm height");
        return false;
    }
    h = (unsigned)strtoul(token, nullptr, 10);
    if(!mesh_photo_read_ppm_token(fp, token, sizeof(token))) {
        fclose(fp);
        mesh_photo_set_error(errbuf, errbuf_len, "missing ppm maxval");
        return false;
    }
    maxval = (unsigned)strtoul(token, nullptr, 10);
    if(w == 0U || h == 0U || w > 8192U || h > 8192U ||
       maxval != 255U) {
        fclose(fp);
        mesh_photo_set_error(errbuf, errbuf_len,
                             "unsupported ppm %ux%u max=%u", w, h, maxval);
        return false;
    }
    want = (size_t)w * (size_t)h * 3U;
    data_len = ftell(fp);
    (void)data_len;
    rgb->assign(want, 0U);
    got = fread(rgb->data(), 1U, want, fp);
    fclose(fp);
    if(got != want) {
        rgb->clear();
        mesh_photo_set_error(errbuf, errbuf_len,
                             "ppm truncated got=%u want=%u",
                             (unsigned)got, (unsigned)want);
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

static bool mesh_photo_read_jpeg_rgb(const char *path,
                                     std::vector<uint8_t> *rgb,
                                     unsigned *width, unsigned *height,
                                     char *errbuf, size_t errbuf_len)
{
    FILE *fp = nullptr;
    struct jpeg_decompress_struct cinfo;
    mesh_jpeg_error_mgr_t jerr;
    uint8_t *row = nullptr;
    bool created = false;
    bool ok = false;

    if(!path || !rgb || !width || !height) {
        mesh_photo_set_error(errbuf, errbuf_len, "invalid jpeg argument");
        return false;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        mesh_photo_set_error(errbuf, errbuf_len, "open failed: %s",
                             strerror(errno));
        return false;
    }

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = mesh_jpeg_error_exit;
    if(setjmp(jerr.setjmp_buffer)) {
        mesh_photo_set_error(errbuf, errbuf_len, "jpeg decode failed");
        goto out;
    }

    jpeg_create_decompress(&cinfo);
    created = true;
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    if(cinfo.output_width == 0U || cinfo.output_height == 0U ||
       cinfo.output_width > 8192U || cinfo.output_height > 8192U ||
       (cinfo.output_components != 1 && cinfo.output_components != 3 &&
        cinfo.output_components != 4)) {
        mesh_photo_set_error(errbuf, errbuf_len,
                             "unsupported jpeg %ux%u components=%u",
                             cinfo.output_width, cinfo.output_height,
                             cinfo.output_components);
        goto out;
    }

    row = (uint8_t *)malloc((size_t)cinfo.output_width *
                            cinfo.output_components);
    if(!row) {
        mesh_photo_set_error(errbuf, errbuf_len, "jpeg row alloc failed");
        goto out;
    }
    rgb->assign((size_t)cinfo.output_width * cinfo.output_height * 3U, 0U);
    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_ptr[1] = { row };
        unsigned y = cinfo.output_scanline;

        jpeg_read_scanlines(&cinfo, row_ptr, 1);
        for(unsigned x = 0; x < cinfo.output_width; x++) {
            size_t dst = ((size_t)y * cinfo.output_width + x) * 3U;
            size_t src = (size_t)x * cinfo.output_components;

            if(cinfo.output_components == 1) {
                (*rgb)[dst + 0U] = row[src];
                (*rgb)[dst + 1U] = row[src];
                (*rgb)[dst + 2U] = row[src];
            } else {
                (*rgb)[dst + 0U] = row[src + 0U];
                (*rgb)[dst + 1U] = row[src + 1U];
                (*rgb)[dst + 2U] = row[src + 2U];
            }
        }
    }
    jpeg_finish_decompress(&cinfo);
    *width = cinfo.output_width;
    *height = cinfo.output_height;
    ok = true;

out:
    free(row);
    if(created) {
        jpeg_destroy_decompress(&cinfo);
    }
    if(fp) {
        fclose(fp);
    }
    if(!ok) {
        rgb->clear();
    }
    return ok;
}

static void mesh_photo_scale_to_canvas(const std::vector<uint8_t> &src,
                                       unsigned src_w, unsigned src_h,
                                       std::vector<uint8_t> *dst,
                                       unsigned dst_w, unsigned dst_h)
{
    unsigned draw_w = dst_w;
    unsigned draw_h = dst_h;
    unsigned off_x;
    unsigned off_y;

    dst->assign((size_t)dst_w * (size_t)dst_h * 3U, 0U);
    if(src_w == 0U || src_h == 0U || src.empty()) {
        return;
    }
    if((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w) {
        draw_h = (unsigned)(((uint64_t)src_h * dst_w) / src_w);
        if(draw_h == 0U) {
            draw_h = 1U;
        }
    } else {
        draw_w = (unsigned)(((uint64_t)src_w * dst_h) / src_h);
        if(draw_w == 0U) {
            draw_w = 1U;
        }
    }
    off_x = (dst_w - draw_w) / 2U;
    off_y = (dst_h - draw_h) / 2U;
    for(unsigned y = 0U; y < draw_h; y++) {
        unsigned sy = (unsigned)(((uint64_t)y * src_h) / draw_h);
        if(sy >= src_h) {
            sy = src_h - 1U;
        }
        for(unsigned x = 0U; x < draw_w; x++) {
            unsigned sx = (unsigned)(((uint64_t)x * src_w) / draw_w);
            size_t src_off;
            size_t dst_off;

            if(sx >= src_w) {
                sx = src_w - 1U;
            }
            src_off = ((size_t)sy * src_w + sx) * 3U;
            dst_off = ((size_t)(off_y + y) * dst_w + (off_x + x)) * 3U;
            (*dst)[dst_off + 0U] = src[src_off + 0U];
            (*dst)[dst_off + 1U] = src[src_off + 1U];
            (*dst)[dst_off + 2U] = src[src_off + 2U];
        }
    }
}

static bool mesh_photo_encode_jpeg_rgb(const std::vector<uint8_t> &rgb,
                                       unsigned width, unsigned height,
                                       int quality,
                                       std::vector<uint8_t> *jpeg,
                                       char *errbuf, size_t errbuf_len)
{
    struct jpeg_compress_struct cinfo;
    mesh_jpeg_error_mgr_t jerr;
    unsigned char *mem = nullptr;
    unsigned long mem_size = 0UL;

    if(!jpeg || rgb.empty() || width == 0U || height == 0U) {
        mesh_photo_set_error(errbuf, errbuf_len, "invalid jpeg source");
        return false;
    }
    jpeg->clear();
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = mesh_jpeg_error_exit;
    if(setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        if(mem) {
            free(mem);
        }
        mesh_photo_set_error(errbuf, errbuf_len, "jpeg encode failed");
        return false;
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &mem_size);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.optimize_coding = TRUE;
    jpeg_start_compress(&cinfo, TRUE);
    while(cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer[1];
        row_pointer[0] =
            (JSAMPROW)&rgb[(size_t)cinfo.next_scanline * width * 3U];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg->assign(mem, mem + mem_size);
    jpeg_destroy_compress(&cinfo);
    free(mem);
    return !jpeg->empty();
}

static bool mesh_photo_write_file(const char *path,
                                  const std::vector<uint8_t> &data,
                                  char *errbuf, size_t errbuf_len)
{
    FILE *fp;

    if(!path || data.empty()) {
        mesh_photo_set_error(errbuf, errbuf_len, "invalid file write");
        return false;
    }
    fp = fopen(path, "wb");
    if(!fp) {
        mesh_photo_set_error(errbuf, errbuf_len, "create failed: %s",
                             strerror(errno));
        return false;
    }
    if(fwrite(data.data(), 1U, data.size(), fp) != data.size()) {
        fclose(fp);
        unlink(path);
        mesh_photo_set_error(errbuf, errbuf_len, "write failed: %s",
                             strerror(errno));
        return false;
    }
    fclose(fp);
    return true;
}

static bool mesh_photo_prepare_jpeg(const char *path, uint32_t stream_id,
                                    std::vector<uint8_t> *jpeg,
                                    std::string *chat_path,
                                    uint16_t *out_w, uint16_t *out_h,
                                    char *errbuf, size_t errbuf_len)
{
    std::vector<uint8_t> src;
    std::vector<uint8_t> canvas;
    unsigned src_w = 0U;
    unsigned src_h = 0U;
    char tmp_path[128];

    if(jpeg) {
        jpeg->clear();
    }
    if(chat_path) {
        chat_path->clear();
    }
    if(out_w) {
        *out_w = MESHTASTIC_FLRC_PHOTO_MAX_W;
    }
    if(out_h) {
        *out_h = MESHTASTIC_FLRC_PHOTO_MAX_H;
    }
    if(!path || !jpeg) {
        mesh_photo_set_error(errbuf, errbuf_len, "invalid photo argument");
        return false;
    }
    if(!mesh_photo_source_path_allowed(path)) {
        mesh_photo_set_error(errbuf, errbuf_len,
                             "photo path must be /root/photos/*.ppm or *.jpg");
        return false;
    }
    if(mesh_photo_path_has_suffix(path, ".ppm")) {
        if(!mesh_photo_read_ppm_rgb(path, &src, &src_w, &src_h, errbuf,
                                    errbuf_len)) {
            return false;
        }
    } else if(!mesh_photo_read_jpeg_rgb(path, &src, &src_w, &src_h, errbuf,
                                        errbuf_len)) {
        return false;
    }
    mesh_photo_scale_to_canvas(src, src_w, src_h, &canvas,
                               MESHTASTIC_FLRC_PHOTO_MAX_W,
                               MESHTASTIC_FLRC_PHOTO_MAX_H);
    for(int quality = MESHTASTIC_FLRC_PHOTO_JPEG_QUALITY;
        quality >= 28; quality -= 7) {
        if(mesh_photo_encode_jpeg_rgb(canvas, MESHTASTIC_FLRC_PHOTO_MAX_W,
                                      MESHTASTIC_FLRC_PHOTO_MAX_H, quality,
                                      jpeg, errbuf, errbuf_len) &&
           jpeg->size() <= MESHTASTIC_FLRC_PHOTO_MAX_BYTES) {
            snprintf(tmp_path, sizeof(tmp_path),
                     "/tmp/k230_mesh_photo_tx_%08x.jpg", stream_id);
            if(mesh_photo_write_file(tmp_path, *jpeg, errbuf, errbuf_len)) {
                if(chat_path) {
                    *chat_path = tmp_path;
                }
                daemon_event("Photo JPEG prepared src=%s stream=0x%08x original=%ux%u encoded=%ux%u bytes=%u quality=%d",
                             path, stream_id, src_w, src_h,
                             MESHTASTIC_FLRC_PHOTO_MAX_W,
                             MESHTASTIC_FLRC_PHOTO_MAX_H,
                             (unsigned)jpeg->size(), quality);
                return true;
            }
            return false;
        }
    }
    mesh_photo_set_error(errbuf, errbuf_len,
                         "jpeg too large: %u bytes max=%u",
                         (unsigned)jpeg->size(),
                         (unsigned)MESHTASTIC_FLRC_PHOTO_MAX_BYTES);
    jpeg->clear();
    return false;
}

typedef struct {
    uint8_t type = 0;
    uint8_t codec_mode = 0;
    uint16_t payload_len = 0;
    uint32_t stream_id = 0;
    uint16_t seq = 0;
    uint16_t total = 0;
    uint32_t total_size = 0;
    uint32_t payload_crc = 0;
    uint32_t stream_crc = 0;
    uint32_t duration_ms = 0;
} mesh_flrc_voice_header_t;

typedef struct {
    uint8_t type = 0;
    uint8_t media_kind = 0;
    uint16_t total = 0;
    uint32_t stream_id = 0;
    uint32_t total_size = 0;
    uint32_t stream_crc = 0;
    uint16_t received_count = 0;
    uint16_t missing_count = 0;
    uint16_t bitmap_len = 0;
    uint8_t round = 0;
    std::vector<uint8_t> missing_bitmap;
} mesh_flrc_photo_repair_request_t;

static void mesh_flrc_voice_make_packet(uint8_t *packet, unsigned packet_len,
                                        uint8_t type, uint32_t stream_id,
                                        uint16_t seq, uint16_t total,
                                        const std::vector<uint8_t> &stream,
                                        const uint8_t *payload,
                                        uint16_t payload_len,
                                        uint32_t duration_ms,
                                        uint8_t codec_mode)
{
    uint32_t payload_crc = payload && payload_len > 0U ?
        crc32_update(0, payload, payload_len) : 0U;
    uint32_t stream_crc = stream.empty() ? 0U :
        crc32_update(0, stream.data(), stream.size());

    memset(packet, 0, packet_len);
    put_le32(packet + 0U, MESHTASTIC_FLRC_VOICE_MAGIC);
    packet[4] = type;
    packet[5] = codec_mode;
    put_le16(packet + 6U, payload_len);
    put_le32(packet + 8U, stream_id);
    put_le16(packet + 12U, seq);
    put_le16(packet + 14U, total);
    put_le32(packet + 16U, (uint32_t)stream.size());
    put_le32(packet + 20U, payload_crc);
    put_le32(packet + 24U, stream_crc);
    put_le32(packet + 28U, duration_ms);
    if(payload && payload_len > 0U &&
       MESHTASTIC_FLRC_VOICE_HDR_LEN + payload_len <= packet_len) {
        memcpy(packet + MESHTASTIC_FLRC_VOICE_HDR_LEN, payload, payload_len);
    }
}

static bool mesh_flrc_voice_parse_packet(const uint8_t *packet,
                                         unsigned packet_len,
                                         mesh_flrc_voice_header_t *hdr)
{
    if(!packet || !hdr || packet_len < MESHTASTIC_FLRC_VOICE_HDR_LEN ||
       get_le32(packet + 0U) != MESHTASTIC_FLRC_VOICE_MAGIC) {
        return false;
    }
    hdr->type = packet[4];
    hdr->codec_mode = packet[5];
    hdr->payload_len = get_le16(packet + 6U);
    hdr->stream_id = get_le32(packet + 8U);
    hdr->seq = get_le16(packet + 12U);
    hdr->total = get_le16(packet + 14U);
    hdr->total_size = get_le32(packet + 16U);
    hdr->payload_crc = get_le32(packet + 20U);
    hdr->stream_crc = get_le32(packet + 24U);
    hdr->duration_ms = get_le32(packet + 28U);
    if(hdr->type < MESHTASTIC_FLRC_VOICE_TYPE_INVITE ||
       hdr->type > MESHTASTIC_FLRC_VOICE_TYPE_DONE ||
       hdr->payload_len > packet_len - MESHTASTIC_FLRC_VOICE_HDR_LEN ||
       hdr->stream_id == 0U || hdr->total == 0U) {
        return false;
    }
    if(hdr->type == MESHTASTIC_FLRC_VOICE_TYPE_DATA &&
       hdr->seq >= hdr->total) {
        return false;
    }
    if(hdr->payload_len > 0U &&
       crc32_update(0, packet + MESHTASTIC_FLRC_VOICE_HDR_LEN,
                    hdr->payload_len) != hdr->payload_crc) {
        return false;
    }
    return true;
}

static bool mesh_flrc_voice_payload_is_invite(
    const std::vector<uint8_t> &payload, mesh_flrc_voice_header_t *hdr)
{
    mesh_flrc_voice_header_t parsed;

    if(!mesh_flrc_voice_parse_packet(payload.data(),
                                     (unsigned)payload.size(), &parsed) ||
       parsed.type != MESHTASTIC_FLRC_VOICE_TYPE_INVITE ||
       parsed.payload_len != 0U ||
       parsed.codec_mode == MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG ||
       parsed.total > 64U ||
       parsed.total_size == 0U) {
        return false;
    }
    if(hdr) {
        *hdr = parsed;
    }
    return true;
}

static std::vector<uint8_t> mesh_flrc_voice_make_invite_payload(
    uint32_t stream_id, const std::vector<uint8_t> &stream,
    uint32_t duration_ms, uint16_t total_packets, uint8_t codec_mode)
{
    std::vector<uint8_t> payload(MESHTASTIC_FLRC_VOICE_HDR_LEN, 0U);

    mesh_flrc_voice_make_packet(payload.data(),
                                MESHTASTIC_FLRC_VOICE_HDR_LEN,
                                MESHTASTIC_FLRC_VOICE_TYPE_INVITE,
                                stream_id, 0U, total_packets, stream,
                                nullptr, 0U, duration_ms, codec_mode);
    return payload;
}

static bool mesh_flrc_photo_payload_is_invite(
    const std::vector<uint8_t> &payload, mesh_flrc_voice_header_t *hdr)
{
    mesh_flrc_voice_header_t parsed;

    if(!mesh_flrc_voice_parse_packet(payload.data(),
                                     (unsigned)payload.size(), &parsed) ||
       parsed.type != MESHTASTIC_FLRC_VOICE_TYPE_INVITE ||
       parsed.payload_len != 0U ||
       parsed.codec_mode != MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG ||
       parsed.total > MESHTASTIC_FLRC_PHOTO_MAX_PACKETS ||
       parsed.total_size == 0U ||
       parsed.total_size > MESHTASTIC_FLRC_PHOTO_MAX_BYTES ||
       parsed.total_size >
       parsed.total * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN) {
        return false;
    }
    if(hdr) {
        *hdr = parsed;
    }
    return true;
}

static std::vector<uint8_t> mesh_flrc_photo_make_invite_payload(
    uint32_t stream_id, const std::vector<uint8_t> &stream,
    uint16_t width, uint16_t height, uint16_t total_packets)
{
    std::vector<uint8_t> payload(MESHTASTIC_FLRC_VOICE_HDR_LEN, 0U);
    uint32_t dims = ((uint32_t)width << 16U) | height;

    mesh_flrc_voice_make_packet(payload.data(),
                                MESHTASTIC_FLRC_VOICE_HDR_LEN,
                                MESHTASTIC_FLRC_VOICE_TYPE_INVITE,
                                stream_id, 0U, total_packets, stream,
                                nullptr, 0U, dims,
                                MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG);
    return payload;
}

static uint16_t mesh_flrc_photo_missing_count(
    const std::vector<uint8_t> &received, uint16_t total)
{
    uint16_t missing = 0U;

    for(uint16_t seq = 0U; seq < total && seq < received.size(); seq++) {
        if(!received[seq]) {
            missing++;
        }
    }
    return missing;
}

static std::vector<uint8_t> mesh_flrc_photo_make_repair_request_payload(
    const mesh_flrc_voice_header_t &invite,
    const std::vector<uint8_t> &received,
    uint16_t received_count, uint8_t round)
{
    uint16_t total = invite.total;
    uint16_t bitmap_len = (uint16_t)((total + 7U) / 8U);
    uint16_t missing = mesh_flrc_photo_missing_count(received, total);
    std::vector<uint8_t> payload(MESHTASTIC_FLRC_PHOTO_REPAIR_HDR_LEN +
                                 bitmap_len, 0U);

    put_le32(payload.data() + 0U, MESHTASTIC_FLRC_PHOTO_REPAIR_MAGIC);
    payload[4] = MESHTASTIC_FLRC_PHOTO_REPAIR_TYPE_REQ;
    payload[5] = MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG;
    put_le16(payload.data() + 6U, total);
    put_le32(payload.data() + 8U, invite.stream_id);
    put_le32(payload.data() + 12U, invite.total_size);
    put_le32(payload.data() + 16U, invite.stream_crc);
    put_le16(payload.data() + 20U, received_count);
    put_le16(payload.data() + 22U, missing);
    put_le16(payload.data() + 24U, bitmap_len);
    payload[26] = round;
    payload[27] = 0U;
    for(uint16_t seq = 0U; seq < total && seq < received.size(); seq++) {
        if(!received[seq]) {
            payload[MESHTASTIC_FLRC_PHOTO_REPAIR_HDR_LEN + (seq / 8U)] |=
                (uint8_t)(1U << (seq % 8U));
        }
    }
    return payload;
}

static bool mesh_flrc_photo_parse_repair_request_payload(
    const std::vector<uint8_t> &payload,
    mesh_flrc_photo_repair_request_t *request)
{
    uint16_t bitmap_len;
    uint16_t total;

    if(!request || payload.size() < MESHTASTIC_FLRC_PHOTO_REPAIR_HDR_LEN ||
       get_le32(payload.data() + 0U) != MESHTASTIC_FLRC_PHOTO_REPAIR_MAGIC ||
       payload[4] != MESHTASTIC_FLRC_PHOTO_REPAIR_TYPE_REQ ||
       payload[5] != MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG) {
        return false;
    }
    total = get_le16(payload.data() + 6U);
    bitmap_len = get_le16(payload.data() + 24U);
    if(total == 0U || total > MESHTASTIC_FLRC_PHOTO_MAX_PACKETS ||
       bitmap_len == 0U || bitmap_len != (uint16_t)((total + 7U) / 8U) ||
       payload.size() != MESHTASTIC_FLRC_PHOTO_REPAIR_HDR_LEN + bitmap_len) {
        return false;
    }

    request->type = payload[4];
    request->media_kind = payload[5];
    request->total = total;
    request->stream_id = get_le32(payload.data() + 8U);
    request->total_size = get_le32(payload.data() + 12U);
    request->stream_crc = get_le32(payload.data() + 16U);
    request->received_count = get_le16(payload.data() + 20U);
    request->missing_count = get_le16(payload.data() + 22U);
    request->bitmap_len = bitmap_len;
    request->round = payload[26];
    if(request->stream_id == 0U || request->total_size == 0U ||
       request->total_size > MESHTASTIC_FLRC_PHOTO_MAX_BYTES ||
       request->total_size >
       request->total * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN ||
       request->missing_count == 0U ||
       request->missing_count >
       request->total - std::min(request->received_count, request->total)) {
        return false;
    }
    request->missing_bitmap.assign(
        payload.begin() + MESHTASTIC_FLRC_PHOTO_REPAIR_HDR_LEN,
        payload.end());
    {
        uint16_t actual_missing = 0U;

        for(uint16_t seq = 0U; seq < request->total; seq++) {
            if((request->missing_bitmap[seq / 8U] &
                (1U << (seq % 8U))) != 0U) {
                actual_missing++;
            }
        }
        if(actual_missing != request->missing_count) {
            return false;
        }
    }
    return true;
}

static bool mesh_flrc_photo_repair_seq_missing(
    const mesh_flrc_photo_repair_request_t &request, uint16_t seq)
{
    if(seq >= request.total ||
       seq / 8U >= request.missing_bitmap.size()) {
        return false;
    }
    return (request.missing_bitmap[seq / 8U] & (1U << (seq % 8U))) != 0U;
}

static int mesh_flrc_voice_should_retry_xtal(int16_t state)
{
    return state == RADIOLIB_ERR_SPI_CMD_INVALID ||
           state == RADIOLIB_ERR_SPI_CMD_FAILED;
}

static int16_t mesh_flrc_voice_set_hf_power(LR2021 *lr2021, int power)
{
    int safe_power = power;
    int16_t state;

    if(!lr2021) {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }
    if(safe_power < -19) {
        safe_power = -19;
    }
    if(safe_power > 9) {
        safe_power = 9;
    }
    state = lr2021->setOutputPower((int8_t)safe_power);
    if(state == RADIOLIB_ERR_SPI_CMD_INVALID) {
        return RADIOLIB_ERR_NONE;
    }
    return state;
}

static int16_t mesh_flrc_voice_begin(LR2021 *lr2021)
{
    uint8_t sync[MESHTASTIC_FLRC_VOICE_SYNC_LEN] = {0x2D, 0x01, 0x4B, 0x1D};
    int16_t state;

    if(!lr2021) {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }
    take_radio_events();
    lr2021->clearPacketReceivedAction();
    lr2021->clearPacketSentAction();
    (void)lr2021->standby();
    lr2021->irqDioNum = LORA_LR2021_IRQ_DIO_NUM;
    state = lr2021->beginFLRC(MESHTASTIC_FLRC_VOICE_FREQ_MHZ,
                              MESHTASTIC_FLRC_VOICE_BR_KBPS,
                              RADIOLIB_LR2021_FLRC_CR_3_4,
                              MESHTASTIC_FLRC_VOICE_POWER_DBM,
                              MESHTASTIC_FLRC_VOICE_PREAMBLE,
                              RADIOLIB_SHAPING_0_5, 3.0f);
    if(mesh_flrc_voice_should_retry_xtal(state)) {
        state = lr2021->beginFLRC(MESHTASTIC_FLRC_VOICE_FREQ_MHZ,
                                  MESHTASTIC_FLRC_VOICE_BR_KBPS,
                                  RADIOLIB_LR2021_FLRC_CR_3_4,
                                  MESHTASTIC_FLRC_VOICE_POWER_DBM,
                                  MESHTASTIC_FLRC_VOICE_PREAMBLE,
                                  RADIOLIB_SHAPING_0_5, 0.0f);
    }
    if(state != RADIOLIB_ERR_NONE &&
       state != RADIOLIB_ERR_SPI_CMD_INVALID) {
        return state;
    }
    lr2021->setRfSwitchTable(lr2021_16e8_rf_switch_dio_pins,
                             lr2021_16e8_rf_switch_table);
    state = mesh_flrc_voice_set_hf_power(lr2021,
                                         MESHTASTIC_FLRC_VOICE_POWER_DBM);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = lr2021->setPreambleLength(MESHTASTIC_FLRC_VOICE_PREAMBLE);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = lr2021->setDataShaping(RADIOLIB_SHAPING_0_5);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = lr2021->setSyncWord(sync, sizeof(sync));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = lr2021->fixedPacketLengthMode(MESHTASTIC_FLRC_VOICE_PACKET_LEN);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    return lr2021->setCRC(2);
}

static bool mesh_flrc_voice_restore_lora(chip_type_t chip,
                                         PhysicalLayer *radio,
                                         SX1262 *sx1262,
                                         LR2021 *lr2021,
                                         const probe_profile_t *profile)
{
    int16_t state = RADIOLIB_ERR_UNKNOWN;

    for(unsigned attempt = 1U; attempt <= 4U; attempt++) {
        if(lr2021) {
            (void)lr2021->clearPacketReceivedAction();
            (void)lr2021->clearPacketSentAction();
            (void)lr2021->standby();
        }
        usleep(20000U * attempt);
        state = begin_chip(chip, radio, sx1262, lr2021, profile);
        if(state == RADIOLIB_ERR_NONE) {
            if(attempt > 1U) {
                daemon_event("FLRC voice restore LoRa recovered attempt=%u",
                             attempt);
            }
            return true;
        }
        daemon_event("FLRC voice restore LoRa retry=%u state=%d %s",
                     attempt, state, error_name(state));
    }
    daemon_event("FLRC voice restore LoRa failed final state=%d %s", state,
                 error_name(state));
    return false;
}

static int16_t mesh_flrc_voice_fast_transmit(LR2021 *lr2021,
                                             const uint8_t *packet,
                                             size_t len)
{
    uint64_t start_us;
    uint64_t timeout_us;
    int16_t state;

    if(!lr2021 || !packet || len == 0U) {
        return RADIOLIB_ERR_UNKNOWN;
    }
    take_radio_events();
    lr2021->clearPacketReceivedAction();
    lr2021->setPacketSentAction(radio_event_isr);
    (void)lr2021->clearTxFifo();
    state = lr2021->startTransmit(packet, len);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    timeout_us = ((uint64_t)len * 8ULL * 1000ULL) /
                 MESHTASTIC_FLRC_VOICE_BR_KBPS;
    timeout_us = timeout_us * 6ULL + 30000ULL;
    if(timeout_us < 40000ULL) {
        timeout_us = 40000ULL;
    }
    start_us = monotonic_us();
    while(take_radio_events() == 0U) {
        if(monotonic_us() - start_us > timeout_us) {
            (void)lr2021->finishTransmit();
            return RADIOLIB_ERR_TX_TIMEOUT;
        }
        usleep(500);
    }
    return lr2021->finishTransmit();
}

static bool mesh_lora_sync_transmit(PhysicalLayer *radio,
                                    const tx_frame_t &frame,
                                    const char *tag)
{
    uint64_t start_us;
    uint64_t timeout_us;
    uint32_t airtime_ms;
    int16_t state;
    size_t len = frame.bytes.size();

    if(!radio || len == 0U || len > MESHTASTIC_MAX_LORA_PAYLOAD_LEN) {
        return false;
    }
    active_op = OP_IDLE;
    take_radio_events();
    state = radio->standby();
    if(state == RADIOLIB_ERR_NONE && active_lr2021) {
        int16_t fifo_state = active_lr2021->clearTxFifo();
        if(fifo_state != RADIOLIB_ERR_NONE) {
            daemon_event("%s clear TX FIFO failed state=%d %s",
                         tag ? tag : "LoRa sync TX", fifo_state,
                         error_name(fifo_state));
        }
    }
    if(state == RADIOLIB_ERR_NONE) {
        radio->clearPacketReceivedAction();
        radio->setPacketSentAction(radio_event_isr);
        state = radio->startTransmit(frame.bytes.data(), len);
    }
    if(state != RADIOLIB_ERR_NONE) {
        daemon_event("%s start failed state=%d %s",
                     tag ? tag : "LoRa sync TX", state, error_name(state));
        return false;
    }

    airtime_ms = mesh_radio_airtime_ms(radio, len);
    timeout_us = (uint64_t)airtime_ms * 1000ULL * 4ULL + 500000ULL;
    if(timeout_us < 1500000ULL) {
        timeout_us = 1500000ULL;
    }
    start_us = monotonic_us();
    while(take_radio_events() == 0U) {
        if(monotonic_us() - start_us > timeout_us) {
            (void)radio->finishTransmit();
            daemon_event("%s timeout len=%u airtime_ms=%u",
                         tag ? tag : "LoRa sync TX", (unsigned)len,
                         airtime_ms);
            return false;
        }
        usleep(1000);
    }
    state = radio->finishTransmit();
    if(state != RADIOLIB_ERR_NONE) {
        daemon_event("%s finish failed state=%d %s",
                     tag ? tag : "LoRa sync TX", state, error_name(state));
        return false;
    }
    tx_count++;
    mesh_airtime_log_tx(airtime_ms);
    mesh_history_remember_tx(frame);
    daemon_event("%s done id=0x%08x len=%u airtime=%ums ch_util=%.1f air_tx=%.2f",
                 tag ? tag : "LoRa sync TX", frame.packet_id,
                 (unsigned)len, airtime_ms,
                 mesh_airtime_channel_util_percent(),
                 mesh_airtime_tx_util_percent());
    return true;
}

static bool mesh_flrc_voice_tx_session(PhysicalLayer *radio, chip_type_t chip,
                                       SX1262 *sx1262, LR2021 *lr2021,
                                       const probe_profile_t *profile,
                                       const tx_frame_t &frame)
{
    uint8_t packet[MESHTASTIC_FLRC_VOICE_PACKET_LEN];
    uint64_t start_us;
    uint16_t total = frame.flrc_voice_total_packets;
    int16_t state;
    bool ok = true;

    if(chip != CHIP_LR2021 || !lr2021 ||
       !frame.flrc_voice_after_tx || frame.flrc_voice_payload.empty()) {
        return false;
    }
    active_op = OP_IDLE;
    usleep(MESHTASTIC_FLRC_VOICE_TX_START_DELAY_US);
    state = mesh_flrc_voice_begin(lr2021);
    if(state != RADIOLIB_ERR_NONE) {
        if(!daemon_chat_update_voice_stream_status(frame.flrc_voice_stream_id,
                                                   "init-failed", 0UL)) {
            daemon_chat("TX 0x%08x voice FLRC init failed: %s",
                        frame.from_node, error_name(state));
        }
        (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021,
                                           profile);
        return true;
    }
    daemon_event("FLRC voice TX start stream=0x%08x packets=%u bytes=%u freq=%.1f br=%u power=%d",
                 frame.flrc_voice_stream_id, total,
                 (unsigned)frame.flrc_voice_payload.size(),
                 MESHTASTIC_FLRC_VOICE_FREQ_MHZ,
                 MESHTASTIC_FLRC_VOICE_BR_KBPS,
                 MESHTASTIC_FLRC_VOICE_POWER_DBM);
    start_us = monotonic_us();
    for(unsigned r = 0; r < MESHTASTIC_FLRC_VOICE_START_REPEAT; r++) {
        mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                    MESHTASTIC_FLRC_VOICE_TYPE_INVITE,
                                    frame.flrc_voice_stream_id, 0U, total,
                                    frame.flrc_voice_payload, nullptr, 0U,
                                    frame.flrc_voice_duration_ms,
                                    frame.flrc_voice_codec_mode);
        state = mesh_flrc_voice_fast_transmit(lr2021, packet, sizeof(packet));
        if(state != RADIOLIB_ERR_NONE) {
            ok = false;
            daemon_event("FLRC voice TX invite repeat=%u failed state=%d %s",
                         r + 1U, state, error_name(state));
            break;
        }
        usleep(MESHTASTIC_FLRC_VOICE_PACKET_GAP_US);
    }
    for(uint16_t seq = 0U; ok && seq < total; seq++) {
        size_t offset = (size_t)seq * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN;
        size_t remain = frame.flrc_voice_payload.size() - offset;
        uint16_t payload_len =
            (uint16_t)std::min(remain,
                               (size_t)MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN);

        mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                    MESHTASTIC_FLRC_VOICE_TYPE_DATA,
                                    frame.flrc_voice_stream_id, seq, total,
                                    frame.flrc_voice_payload,
                                    frame.flrc_voice_payload.data() + offset,
                                    payload_len,
                                    frame.flrc_voice_duration_ms,
                                    frame.flrc_voice_codec_mode);
        for(unsigned r = 0; r < MESHTASTIC_FLRC_VOICE_DATA_REPEAT; r++) {
            state = mesh_flrc_voice_fast_transmit(lr2021, packet,
                                                  sizeof(packet));
            if(state != RADIOLIB_ERR_NONE) {
                ok = false;
                daemon_event("FLRC voice TX data seq=%u/%u repeat=%u failed state=%d %s",
                             seq + 1U, total, r + 1U, state,
                             error_name(state));
                break;
            }
            usleep(MESHTASTIC_FLRC_VOICE_PACKET_GAP_US);
        }
    }
    for(unsigned r = 0; ok && r < MESHTASTIC_FLRC_VOICE_DONE_REPEAT; r++) {
        mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                    MESHTASTIC_FLRC_VOICE_TYPE_DONE,
                                    frame.flrc_voice_stream_id, total - 1U,
                                    total, frame.flrc_voice_payload, nullptr,
                                    0U, frame.flrc_voice_duration_ms,
                                    frame.flrc_voice_codec_mode);
        state = mesh_flrc_voice_fast_transmit(lr2021, packet, sizeof(packet));
        if(state != RADIOLIB_ERR_NONE) {
            ok = false;
            daemon_event("FLRC voice TX done repeat=%u failed state=%d %s",
                         r + 1U, state, error_name(state));
            break;
        }
        usleep(MESHTASTIC_FLRC_VOICE_PACKET_GAP_US);
    }
    if(ok) {
        uint64_t elapsed_ms = (monotonic_us() - start_us) / 1000ULL;
        mesh_voice_tx_stream_count++;
        mesh_voice_tx_chunk_count += total;
        if(!daemon_chat_update_voice_stream_status(frame.flrc_voice_stream_id,
                                                   "sent",
                                                   (unsigned long)elapsed_ms)) {
            daemon_chat("TX 0x%08x voice %.1fs codec=codec2-flrc packets=%u stream=0x%08x state=sent elapsed=%lums",
                        frame.from_node,
                        (double)frame.flrc_voice_duration_ms / 1000.0,
                        total, frame.flrc_voice_stream_id,
                        (unsigned long)elapsed_ms);
        }
        daemon_event("FLRC voice TX done stream=0x%08x elapsed_ms=%lu",
                     frame.flrc_voice_stream_id, (unsigned long)elapsed_ms);
    } else {
        if(!daemon_chat_update_voice_stream_status(frame.flrc_voice_stream_id,
                                                   "failed", 0UL)) {
            daemon_chat("TX 0x%08x voice FLRC failed stream=0x%08x",
                        frame.from_node, frame.flrc_voice_stream_id);
        }
    }
    (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021, profile);
    return true;
}

static bool mesh_flrc_voice_rx_session(const probe_options_t &opts,
                                       PhysicalLayer *radio,
                                       chip_type_t chip, SX1262 *sx1262,
                                       LR2021 *lr2021,
                                       const probe_profile_t *profile,
                                       const mesh_header_t &mesh_header,
                                       const mesh_flrc_voice_header_t &invite,
                                       float control_rssi)
{
    uint8_t packet[MESHTASTIC_FLRC_VOICE_PACKET_LEN];
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> received;
    std::vector<uint8_t> stream;
    uint64_t timeout_us;
    uint64_t start_us;
    uint16_t received_count = 0U;
    int16_t state;

    if(chip != CHIP_LR2021 || !lr2021) {
        daemon_event("FLRC voice invite ignored from=0x%08x reason=not-lr2021 chip=%s",
                     mesh_header.from, chip_name(chip));
        return false;
    }
    if(invite.total == 0U || invite.total > 64U ||
       invite.total_size == 0U ||
       invite.total_size >
       invite.total * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN) {
        daemon_event("FLRC voice invite invalid from=0x%08x stream=0x%08x packets=%u size=%u",
                     mesh_header.from, invite.stream_id, invite.total,
                     invite.total_size);
        return false;
    }
    active_op = OP_IDLE;
    state = mesh_flrc_voice_begin(lr2021);
    if(state != RADIOLIB_ERR_NONE) {
        daemon_chat("RX 0x%08x voice FLRC init failed: %s",
                    mesh_header.from, error_name(state));
        (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021,
                                           profile);
        return true;
    }
    chunks.assign(invite.total, std::vector<uint8_t>());
    received.assign(invite.total, 0U);
    timeout_us = ((uint64_t)invite.duration_ms +
                  MESHTASTIC_FLRC_VOICE_RX_GUARD_MS) * 1000ULL;
    if(timeout_us < 5000000ULL) {
        timeout_us = 5000000ULL;
    }
    start_us = monotonic_us();
    daemon_event("FLRC voice RX window from=0x%08x stream=0x%08x packets=%u bytes=%u timeout_ms=%lu control_rssi=%.1f",
                 mesh_header.from, invite.stream_id, invite.total,
                 invite.total_size, (unsigned long)(timeout_us / 1000ULL),
                 control_rssi);
    while(monotonic_us() - start_us < timeout_us &&
          received_count < invite.total) {
        mesh_flrc_voice_header_t hdr;
        size_t payload_len;

        take_radio_events();
        lr2021->clearPacketSentAction();
        lr2021->setPacketReceivedAction(radio_event_isr);
        state = lr2021->startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF,
                                     RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
                                     RADIOLIB_IRQ_RX_DEFAULT_MASK,
                                     MESHTASTIC_FLRC_VOICE_PACKET_LEN);
        if(state != RADIOLIB_ERR_NONE) {
            daemon_event("FLRC voice RX start failed state=%d %s",
                         state, error_name(state));
            break;
        }
        while(take_radio_events() == 0U) {
            if(monotonic_us() - start_us >= timeout_us) {
                break;
            }
            usleep(1000);
        }
        if(monotonic_us() - start_us >= timeout_us) {
            (void)lr2021->standby();
            break;
        }
        state = lr2021->readData(packet, MESHTASTIC_FLRC_VOICE_PACKET_LEN);
        (void)lr2021->finishReceive();
        if(state != RADIOLIB_ERR_NONE) {
            daemon_event("FLRC voice RX read failed state=%d %s",
                         state, error_name(state));
            continue;
        }
        if(!mesh_flrc_voice_parse_packet(packet,
                                         MESHTASTIC_FLRC_VOICE_PACKET_LEN,
                                         &hdr) ||
           hdr.stream_id != invite.stream_id ||
           hdr.total != invite.total) {
            continue;
        }
        if(hdr.type == MESHTASTIC_FLRC_VOICE_TYPE_DONE) {
            daemon_event("FLRC voice RX done marker stream=0x%08x received=%u/%u",
                         invite.stream_id, received_count, invite.total);
            continue;
        }
        if(hdr.type != MESHTASTIC_FLRC_VOICE_TYPE_DATA ||
           hdr.seq >= invite.total || received[hdr.seq]) {
            continue;
        }
        payload_len = hdr.payload_len;
        chunks[hdr.seq].assign(packet + MESHTASTIC_FLRC_VOICE_HDR_LEN,
                               packet + MESHTASTIC_FLRC_VOICE_HDR_LEN +
                                   payload_len);
        received[hdr.seq] = 1U;
        received_count++;
        mesh_voice_rx_chunk_count++;
        daemon_event("FLRC voice RX data stream=0x%08x seq=%u/%u len=%u rssi=%.1f",
                     invite.stream_id, hdr.seq + 1U, invite.total,
                     (unsigned)payload_len, lr2021->getRSSI());
    }
    (void)lr2021->standby();
    if(received_count == invite.total) {
        char path[128];
        char errbuf[128];
        unsigned duration_ms = 0U;

        stream.reserve(invite.total_size);
        for(uint16_t seq = 0U; seq < invite.total; seq++) {
            stream.insert(stream.end(), chunks[seq].begin(), chunks[seq].end());
        }
        if(stream.size() > invite.total_size) {
            stream.resize(invite.total_size);
        }
        if((uint32_t)stream.size() == invite.total_size &&
           crc32_update(0, stream.data(), stream.size()) == invite.stream_crc) {
            snprintf(path, sizeof(path),
                     "/tmp/k230_mesh_voice_rx_%08x_%08x.raw",
                     mesh_header.from, invite.stream_id);
            if(mesh_voice_decode_codec2_payload_to_file(stream, path,
                                                        &duration_ms,
                                                        errbuf,
                                                        sizeof(errbuf))) {
                mesh_voice_rx_complete_count++;
                daemon_chat("RX 0x%08x voice %.1fs codec=codec2-flrc packets=%u rssi=%ddBm file=%s",
                            mesh_header.from,
                            (double)duration_ms / 1000.0,
                            invite.total, (int)roundf(control_rssi), path);
            } else {
                mesh_voice_rx_decode_fail_count++;
                daemon_chat("RX 0x%08x FLRC voice decode failed: %s",
                            mesh_header.from, errbuf);
            }
        } else {
            mesh_voice_rx_decode_fail_count++;
            daemon_chat("RX 0x%08x FLRC voice crc failed packets=%u/%u",
                        mesh_header.from, received_count, invite.total);
        }
    } else {
        mesh_voice_rx_decode_fail_count++;
        daemon_chat("RX 0x%08x FLRC voice incomplete packets=%u/%u",
                    mesh_header.from, received_count, invite.total);
    }
    (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021, profile);
    return true;
}

static bool mesh_photo_store_dir_ensure(void)
{
    if(mkdir("/root/meshtastic", 0755) != 0 && errno != EEXIST) {
        daemon_event("Photo store mkdir /root/meshtastic failed: %s",
                     strerror(errno));
        return false;
    }
    if(mkdir(MESHTASTIC_PHOTO_STORE_DIR, 0755) != 0 &&
       errno != EEXIST) {
        daemon_event("Photo store mkdir %s failed: %s",
                     MESHTASTIC_PHOTO_STORE_DIR, strerror(errno));
        return false;
    }
    return true;
}

static void mesh_photo_debug_drop_seq_text(char *buf, size_t buflen)
{
    size_t off = 0U;

    if(!buf || buflen == 0U) {
        return;
    }
    buf[0] = '\0';
    if(mesh_photo_debug_drop.seq_count == 0U) {
        snprintf(buf, buflen, "%s", "-");
        return;
    }
    for(uint16_t i = 0; i < mesh_photo_debug_drop.seq_count; i++) {
        int written = snprintf(buf + off, buflen - off, "%s%u",
                               i == 0U ? "" : ",",
                               mesh_photo_debug_drop.seq[i]);
        if(written < 0) {
            break;
        }
        if((size_t)written >= buflen - off) {
            off = buflen - 1U;
            break;
        }
        off += (size_t)written;
    }
}

static std::string mesh_photo_debug_drop_status_response(void)
{
    char seq_text[256];
    char buf[384];

    mesh_photo_debug_drop_seq_text(seq_text, sizeof(seq_text));
    snprintf(buf, sizeof(buf),
             "OK photo_drop=%s photo_drop_seq=%s photo_drop_rate=%u "
             "photo_drop_hits=%lu\n",
             mesh_photo_debug_drop.enabled ? "on" : "off", seq_text,
             (unsigned)mesh_photo_debug_drop.rate_percent,
             (unsigned long)mesh_photo_debug_drop.hit_count);
    return std::string(buf);
}

static bool mesh_photo_debug_drop_has_seq(uint16_t display_seq)
{
    for(uint16_t i = 0; i < mesh_photo_debug_drop.seq_count; i++) {
        if(mesh_photo_debug_drop.seq[i] == display_seq) {
            return true;
        }
    }
    return false;
}

static uint32_t mesh_photo_debug_drop_hash(uint32_t stream_id,
                                           uint16_t display_seq,
                                           uint16_t total)
{
    uint32_t x = stream_id ^ 0x9e3779b9U;

    x ^= (uint32_t)display_seq * 0x85ebca6bU;
    x ^= (uint32_t)total * 0xc2b2ae35U;
    x ^= x >> 16U;
    x *= 0x7feb352dU;
    x ^= x >> 15U;
    x *= 0x846ca68bU;
    x ^= x >> 16U;
    return x;
}

static bool mesh_photo_debug_drop_should_drop(uint32_t stream_id,
                                              uint16_t seq0,
                                              uint16_t total,
                                              bool repair)
{
    uint16_t display_seq = (uint16_t)(seq0 + 1U);
    bool drop = false;

    if(repair || !mesh_photo_debug_drop.enabled) {
        return false;
    }
    if(mesh_photo_debug_drop_has_seq(display_seq)) {
        drop = true;
    }
    if(!drop && mesh_photo_debug_drop.rate_percent > 0U) {
        drop = (mesh_photo_debug_drop_hash(stream_id, display_seq, total) %
                100U) < mesh_photo_debug_drop.rate_percent;
    }
    if(drop) {
        mesh_photo_debug_drop.hit_count++;
        daemon_event("FLRC photo debug drop stream=0x%08x seq=%u/%u "
                     "rate=%u hits=%lu",
                     stream_id, display_seq, total,
                     (unsigned)mesh_photo_debug_drop.rate_percent,
                     (unsigned long)mesh_photo_debug_drop.hit_count);
    }
    return drop;
}

static std::string mesh_photo_debug_drop_clear_response(void)
{
    memset(&mesh_photo_debug_drop, 0, sizeof(mesh_photo_debug_drop));
    return mesh_photo_debug_drop_status_response();
}

static std::string mesh_photo_debug_drop_set_seq_response(const char *arg)
{
    const char *p = arg ? arg : "";
    uint16_t parsed[MESHTASTIC_FLRC_PHOTO_DEBUG_DROP_MAX_SEQ];
    uint16_t parsed_count = 0U;

    while(*p) {
        char *endp = NULL;
        unsigned long val;

        while(*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if(!*p) {
            break;
        }
        val = strtoul(p, &endp, 10);
        if(endp == p || val == 0UL ||
           val > MESHTASTIC_FLRC_PHOTO_MAX_PACKETS) {
            return "ERR invalid-photo-drop-seq\n";
        }
        if(parsed_count >= MESHTASTIC_FLRC_PHOTO_DEBUG_DROP_MAX_SEQ) {
            return "ERR too-many-photo-drop-seq\n";
        }
        parsed[parsed_count++] = (uint16_t)val;
        p = endp;
        while(*p && !isspace((unsigned char)*p) && *p != ',') {
            return "ERR invalid-photo-drop-seq\n";
        }
    }
    std::sort(parsed, parsed + parsed_count);
    mesh_photo_debug_drop.seq_count = 0U;
    for(uint16_t i = 0; i < parsed_count; i++) {
        if(i > 0U && parsed[i] == parsed[i - 1U]) {
            continue;
        }
        mesh_photo_debug_drop.seq[mesh_photo_debug_drop.seq_count++] =
            parsed[i];
    }
    mesh_photo_debug_drop.enabled =
        mesh_photo_debug_drop.seq_count > 0U ||
        mesh_photo_debug_drop.rate_percent > 0U;
    return mesh_photo_debug_drop_status_response();
}

static std::string mesh_photo_debug_drop_set_rate_response(const char *arg)
{
    const char *p = arg ? arg : "";
    char *endp = NULL;
    unsigned long val;

    while(*p && isspace((unsigned char)*p)) {
        p++;
    }
    val = strtoul(p, &endp, 10);
    if(endp == p || val > 100UL) {
        return "ERR invalid-photo-drop-rate\n";
    }
    while(*endp && isspace((unsigned char)*endp)) {
        endp++;
    }
    if(*endp) {
        return "ERR invalid-photo-drop-rate\n";
    }
    mesh_photo_debug_drop.rate_percent = (uint8_t)val;
    mesh_photo_debug_drop.enabled =
        mesh_photo_debug_drop.seq_count > 0U ||
        mesh_photo_debug_drop.rate_percent > 0U;
    return mesh_photo_debug_drop_status_response();
}

static void mesh_media_config_clamp(void)
{
    if(mesh_media_cfg.photo_data_repeat < 1U) {
        mesh_media_cfg.photo_data_repeat = 1U;
    } else if(mesh_media_cfg.photo_data_repeat > 3U) {
        mesh_media_cfg.photo_data_repeat = 3U;
    }
    if(mesh_media_cfg.photo_repair_rounds > 4U) {
        mesh_media_cfg.photo_repair_rounds = 4U;
    }
    if(mesh_media_cfg.photo_repair_repeat < 1U) {
        mesh_media_cfg.photo_repair_repeat = 1U;
    } else if(mesh_media_cfg.photo_repair_repeat > 3U) {
        mesh_media_cfg.photo_repair_repeat = 3U;
    }
    if(mesh_media_cfg.photo_repair_window_ms < 3000U) {
        mesh_media_cfg.photo_repair_window_ms = 3000U;
    } else if(mesh_media_cfg.photo_repair_window_ms > 20000U) {
        mesh_media_cfg.photo_repair_window_ms = 20000U;
    }
    if(mesh_media_cfg.photo_tx_cache_ttl_sec < 60U) {
        mesh_media_cfg.photo_tx_cache_ttl_sec = 60U;
    } else if(mesh_media_cfg.photo_tx_cache_ttl_sec > 900U) {
        mesh_media_cfg.photo_tx_cache_ttl_sec = 900U;
    }
}

static std::string mesh_media_config_status_response(void)
{
    char buf[240];

    mesh_media_config_clamp();
    snprintf(buf, sizeof(buf),
             "OK photo_repeat=%u repair_rounds=%u repair_repeat=%u "
             "repair_window_ms=%u cache_ttl_sec=%u\n",
             (unsigned)mesh_media_cfg.photo_data_repeat,
             (unsigned)mesh_media_cfg.photo_repair_rounds,
             (unsigned)mesh_media_cfg.photo_repair_repeat,
             (unsigned)mesh_media_cfg.photo_repair_window_ms,
             (unsigned)mesh_media_cfg.photo_tx_cache_ttl_sec);
    return std::string(buf);
}

static bool mesh_media_config_apply_field(const char *key, const char *value)
{
    uint32_t parsed;

    if(!key || !value || !mesh_parse_u32_text(value, &parsed)) {
        return false;
    }
    if(strcmp(key, "photo_repeat") == 0) {
        mesh_media_cfg.photo_data_repeat = (uint8_t)parsed;
    } else if(strcmp(key, "repair_rounds") == 0) {
        mesh_media_cfg.photo_repair_rounds = (uint8_t)parsed;
    } else if(strcmp(key, "repair_repeat") == 0) {
        mesh_media_cfg.photo_repair_repeat = (uint8_t)parsed;
    } else if(strcmp(key, "repair_window_ms") == 0) {
        mesh_media_cfg.photo_repair_window_ms = parsed;
    } else if(strcmp(key, "cache_ttl_sec") == 0) {
        mesh_media_cfg.photo_tx_cache_ttl_sec = parsed;
    } else {
        return false;
    }
    mesh_media_config_clamp();
    return true;
}

static std::string mesh_media_config_set_response(const char *arg)
{
    char copy[256];
    char *save = NULL;
    char *token;

    if(!arg) {
        return mesh_media_config_status_response();
    }
    while(*arg && isspace((unsigned char)*arg)) {
        arg++;
    }
    if(!*arg) {
        return mesh_media_config_status_response();
    }
    snprintf(copy, sizeof(copy), "%s", arg);
    token = strtok_r(copy, " \t\r\n", &save);
    while(token) {
        char *eq = strchr(token, '=');

        if(!eq || eq == token || !eq[1]) {
            return "ERR invalid-media-config\n";
        }
        *eq = '\0';
        if(!mesh_media_config_apply_field(token, eq + 1)) {
            return "ERR invalid-media-config\n";
        }
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    daemon_event("Media config updated photo_repeat=%u repair_rounds=%u repair_repeat=%u repair_window_ms=%u cache_ttl_sec=%u",
                 (unsigned)mesh_media_cfg.photo_data_repeat,
                 (unsigned)mesh_media_cfg.photo_repair_rounds,
                 (unsigned)mesh_media_cfg.photo_repair_repeat,
                 (unsigned)mesh_media_cfg.photo_repair_window_ms,
                 (unsigned)mesh_media_cfg.photo_tx_cache_ttl_sec);
    return mesh_media_config_status_response();
}

static uint16_t mesh_flrc_photo_collect_window(
    LR2021 *lr2021, const mesh_flrc_voice_header_t &invite,
    std::vector<std::vector<uint8_t>> *chunks,
    std::vector<uint8_t> *received, uint16_t *received_count,
    uint64_t timeout_us, const char *phase, bool repair)
{
    uint8_t packet[MESHTASTIC_FLRC_VOICE_PACKET_LEN];
    uint64_t start_us;
    uint16_t before;
    uint16_t added = 0U;
    const char *tag = phase && phase[0] ? phase : "rx";

    if(!lr2021 || !chunks || !received || !received_count ||
       chunks->size() < invite.total || received->size() < invite.total) {
        return 0U;
    }
    before = *received_count;
    start_us = monotonic_us();
    while(monotonic_us() - start_us < timeout_us &&
          *received_count < invite.total) {
        mesh_flrc_voice_header_t hdr;
        size_t payload_len;
        int16_t state;

        take_radio_events();
        lr2021->clearPacketSentAction();
        lr2021->setPacketReceivedAction(radio_event_isr);
        state = lr2021->startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF,
                                     RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
                                     RADIOLIB_IRQ_RX_DEFAULT_MASK,
                                     MESHTASTIC_FLRC_VOICE_PACKET_LEN);
        if(state != RADIOLIB_ERR_NONE) {
            daemon_event("FLRC photo %s start failed state=%d %s",
                         tag, state, error_name(state));
            break;
        }
        while(take_radio_events() == 0U) {
            if(monotonic_us() - start_us >= timeout_us) {
                break;
            }
            usleep(1000);
        }
        if(monotonic_us() - start_us >= timeout_us) {
            (void)lr2021->standby();
            break;
        }
        state = lr2021->readData(packet, MESHTASTIC_FLRC_VOICE_PACKET_LEN);
        (void)lr2021->finishReceive();
        if(state != RADIOLIB_ERR_NONE) {
            daemon_event("FLRC photo %s read failed state=%d %s",
                         tag, state, error_name(state));
            continue;
        }
        if(!mesh_flrc_voice_parse_packet(packet,
                                         MESHTASTIC_FLRC_VOICE_PACKET_LEN,
                                         &hdr) ||
           hdr.stream_id != invite.stream_id ||
           hdr.total != invite.total ||
           hdr.codec_mode != MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG) {
            continue;
        }
        if(hdr.type == MESHTASTIC_FLRC_VOICE_TYPE_DONE) {
            daemon_event("FLRC photo %s done marker stream=0x%08x received=%u/%u",
                         tag, invite.stream_id, *received_count,
                         invite.total);
            continue;
        }
        if(hdr.type != MESHTASTIC_FLRC_VOICE_TYPE_DATA ||
           hdr.seq >= invite.total || (*received)[hdr.seq]) {
            continue;
        }
        if(mesh_photo_debug_drop_should_drop(invite.stream_id, hdr.seq,
                                             invite.total, repair)) {
            continue;
        }
        payload_len = hdr.payload_len;
        (*chunks)[hdr.seq].assign(
            packet + MESHTASTIC_FLRC_VOICE_HDR_LEN,
            packet + MESHTASTIC_FLRC_VOICE_HDR_LEN + payload_len);
        (*received)[hdr.seq] = 1U;
        (*received_count)++;
        if(repair) {
            mesh_photo_repair_rx_chunk_count++;
        } else {
            mesh_photo_rx_chunk_count++;
        }
        daemon_event("FLRC photo %s data stream=0x%08x seq=%u/%u len=%u rssi=%.1f",
                     tag, invite.stream_id, hdr.seq + 1U, invite.total,
                     (unsigned)payload_len, lr2021->getRSSI());
    }
    if(*received_count >= before) {
        added = (uint16_t)(*received_count - before);
    }
    return added;
}

static int16_t mesh_flrc_photo_transmit_seq(
    LR2021 *lr2021, const std::vector<uint8_t> &payload,
    uint32_t stream_id, uint16_t total, uint16_t width, uint16_t height,
    uint16_t seq)
{
    uint8_t packet[MESHTASTIC_FLRC_VOICE_PACKET_LEN];
    size_t offset;
    size_t remain;
    uint16_t payload_len;
    uint32_t dims = ((uint32_t)width << 16U) | height;

    if(!lr2021 || seq >= total || payload.empty()) {
        return RADIOLIB_ERR_UNKNOWN;
    }
    offset = (size_t)seq * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN;
    if(offset >= payload.size()) {
        return RADIOLIB_ERR_PACKET_TOO_LONG;
    }
    remain = payload.size() - offset;
    payload_len = (uint16_t)std::min(
        remain, (size_t)MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN);
    mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                MESHTASTIC_FLRC_VOICE_TYPE_DATA,
                                stream_id, seq, total, payload,
                                payload.data() + offset, payload_len, dims,
                                MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG);
    return mesh_flrc_voice_fast_transmit(lr2021, packet, sizeof(packet));
}

static bool mesh_flrc_photo_repair_tx_session(
    PhysicalLayer *radio, chip_type_t chip, SX1262 *sx1262, LR2021 *lr2021,
    const probe_profile_t *profile, uint32_t requester,
    const mesh_flrc_photo_repair_request_t &request)
{
    uint8_t packet[MESHTASTIC_FLRC_VOICE_PACKET_LEN];
    mesh_photo_tx_cache_entry_t *cache;
    uint32_t dims;
    uint16_t sent = 0U;
    uint16_t requested = 0U;
    int16_t state;
    bool ok = true;

    if(chip != CHIP_LR2021 || !lr2021) {
        daemon_event("FLRC photo repair ignored requester=0x%08x stream=0x%08x reason=not-lr2021 chip=%s",
                     requester, request.stream_id, chip_name(chip));
        return false;
    }
    mesh_photo_repair_req_rx_count++;
    cache = mesh_photo_tx_cache_find(request.stream_id, request.stream_crc,
                                     request.total, request.total_size);
    if(!cache) {
        mesh_photo_repair_fail_count++;
        daemon_event("FLRC photo repair TX no-cache requester=0x%08x stream=0x%08x missing=%u round=%u",
                     requester, request.stream_id, request.missing_count,
                     request.round);
        return true;
    }
    dims = ((uint32_t)cache->width << 16U) | cache->height;
    active_op = OP_IDLE;
    mesh_media_config_clamp();
    usleep(MESHTASTIC_FLRC_PHOTO_REPAIR_START_DELAY_US);
    state = mesh_flrc_voice_begin(lr2021);
    if(state != RADIOLIB_ERR_NONE) {
        mesh_photo_repair_fail_count++;
        daemon_event("FLRC photo repair TX init failed requester=0x%08x stream=0x%08x state=%d %s",
                     requester, request.stream_id, state, error_name(state));
        (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021,
                                           profile);
        return true;
    }
    daemon_event("FLRC photo repair TX start requester=0x%08x stream=0x%08x missing=%u received=%u/%u round=%u repeat=%u",
                 requester, request.stream_id, request.missing_count,
                 request.received_count, request.total, request.round,
                 (unsigned)mesh_media_cfg.photo_repair_repeat);
    for(unsigned pass = 0; ok && pass < mesh_media_cfg.photo_repair_repeat;
        pass++) {
        for(uint16_t seq = 0U; ok && seq < request.total; seq++) {
            if(!mesh_flrc_photo_repair_seq_missing(request, seq)) {
                continue;
            }
            if(pass == 0U) {
                requested++;
            }
            state = mesh_flrc_photo_transmit_seq(
                lr2021, cache->payload, cache->stream_id, cache->total,
                cache->width, cache->height, seq);
            if(state != RADIOLIB_ERR_NONE) {
                ok = false;
                daemon_event("FLRC photo repair TX data pass=%u seq=%u/%u failed state=%d %s",
                             pass + 1U, seq + 1U, request.total,
                             state, error_name(state));
                break;
            }
            sent++;
            mesh_photo_repair_tx_chunk_count++;
            usleep(MESHTASTIC_FLRC_PHOTO_PACKET_GAP_US);
        }
        if(ok && pass + 1U < mesh_media_cfg.photo_repair_repeat) {
            usleep(MESHTASTIC_FLRC_PHOTO_ROUND_GAP_US);
        }
    }
    for(unsigned r = 0; ok && r < MESHTASTIC_FLRC_PHOTO_DONE_REPEAT; r++) {
        mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                    MESHTASTIC_FLRC_VOICE_TYPE_DONE,
                                    cache->stream_id,
                                    cache->total > 0U ? cache->total - 1U : 0U,
                                    cache->total, cache->payload, nullptr, 0U,
                                    dims,
                                    MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG);
        state = mesh_flrc_voice_fast_transmit(lr2021, packet, sizeof(packet));
        if(state != RADIOLIB_ERR_NONE) {
            ok = false;
            daemon_event("FLRC photo repair TX done repeat=%u failed state=%d %s",
                         r + 1U, state, error_name(state));
            break;
        }
        usleep(MESHTASTIC_FLRC_PHOTO_PACKET_GAP_US);
    }
    if(ok) {
        daemon_event("FLRC photo repair TX done requester=0x%08x stream=0x%08x requested=%u sent=%u",
                     requester, request.stream_id, requested, sent);
    } else {
        mesh_photo_repair_fail_count++;
        daemon_event("FLRC photo repair TX failed requester=0x%08x stream=0x%08x requested=%u sent=%u",
                     requester, request.stream_id, requested, sent);
    }
    (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021, profile);
    return true;
}

static bool mesh_flrc_photo_tx_session(PhysicalLayer *radio, chip_type_t chip,
                                       SX1262 *sx1262, LR2021 *lr2021,
                                       const probe_profile_t *profile,
                                       const tx_frame_t &frame)
{
    uint8_t packet[MESHTASTIC_FLRC_VOICE_PACKET_LEN];
    uint64_t start_us;
    uint16_t total = frame.flrc_photo_total_packets;
    uint32_t dims = ((uint32_t)frame.flrc_photo_width << 16U) |
                    frame.flrc_photo_height;
    std::string stream_sha256 =
        mesh_sha256_hex_vector(frame.flrc_photo_payload);
    int16_t state;
    bool ok = true;

    if(chip != CHIP_LR2021 || !lr2021 ||
       !frame.flrc_photo_after_tx || frame.flrc_photo_payload.empty()) {
        return false;
    }
    mesh_photo_tx_cache_store(frame);
    active_op = OP_IDLE;
    usleep(MESHTASTIC_FLRC_VOICE_TX_START_DELAY_US);
    state = mesh_flrc_voice_begin(lr2021);
    if(state != RADIOLIB_ERR_NONE) {
        if(!daemon_chat_update_photo_stream_status(frame.flrc_photo_stream_id,
                                                   "init-failed", 0UL)) {
            daemon_chat("TX 0x%08x photo FLRC init failed: %s",
                        frame.from_node, error_name(state));
        }
        (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021,
                                           profile);
        return true;
    }
    daemon_event("FLRC photo TX start stream=0x%08x packets=%u bytes=%u size=%ux%u freq=%.1f br=%u power=%d",
                 frame.flrc_photo_stream_id, total,
                 (unsigned)frame.flrc_photo_payload.size(),
                 frame.flrc_photo_width, frame.flrc_photo_height,
                 MESHTASTIC_FLRC_VOICE_FREQ_MHZ,
                 MESHTASTIC_FLRC_VOICE_BR_KBPS,
                 MESHTASTIC_FLRC_VOICE_POWER_DBM);
    start_us = monotonic_us();
    mesh_media_config_clamp();
    for(unsigned r = 0; r < MESHTASTIC_FLRC_PHOTO_START_REPEAT; r++) {
        mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                    MESHTASTIC_FLRC_VOICE_TYPE_INVITE,
                                    frame.flrc_photo_stream_id, 0U, total,
                                    frame.flrc_photo_payload, nullptr, 0U,
                                    dims,
                                    MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG);
        state = mesh_flrc_voice_fast_transmit(lr2021, packet, sizeof(packet));
        if(state != RADIOLIB_ERR_NONE) {
            ok = false;
            daemon_event("FLRC photo TX invite repeat=%u failed state=%d %s",
                         r + 1U, state, error_name(state));
            break;
        }
        usleep(MESHTASTIC_FLRC_PHOTO_PACKET_GAP_US);
    }
    for(unsigned pass = 0; ok && pass < mesh_media_cfg.photo_data_repeat;
        pass++) {
        for(uint16_t seq = 0U; ok && seq < total; seq++) {
            size_t offset = (size_t)seq * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN;
            size_t remain = frame.flrc_photo_payload.size() - offset;
            uint16_t payload_len =
                (uint16_t)std::min(remain,
                                   (size_t)MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN);

            mesh_flrc_voice_make_packet(
                packet, sizeof(packet), MESHTASTIC_FLRC_VOICE_TYPE_DATA,
                frame.flrc_photo_stream_id, seq, total,
                frame.flrc_photo_payload,
                frame.flrc_photo_payload.data() + offset,
                payload_len, dims, MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG);
            state = mesh_flrc_voice_fast_transmit(lr2021, packet,
                                                  sizeof(packet));
            if(state != RADIOLIB_ERR_NONE) {
                ok = false;
                daemon_event("FLRC photo TX data pass=%u seq=%u/%u failed state=%d %s",
                             pass + 1U, seq + 1U, total, state,
                             error_name(state));
                break;
            }
            usleep(MESHTASTIC_FLRC_PHOTO_PACKET_GAP_US);
        }
        if(ok && pass + 1U < mesh_media_cfg.photo_data_repeat) {
            daemon_event("FLRC photo TX repeat pass=%u/%u stream=0x%08x",
                         pass + 1U,
                         (unsigned)mesh_media_cfg.photo_data_repeat,
                         frame.flrc_photo_stream_id);
            usleep(MESHTASTIC_FLRC_PHOTO_ROUND_GAP_US);
        }
    }
    for(unsigned r = 0; ok && r < MESHTASTIC_FLRC_PHOTO_DONE_REPEAT; r++) {
        mesh_flrc_voice_make_packet(packet, sizeof(packet),
                                    MESHTASTIC_FLRC_VOICE_TYPE_DONE,
                                    frame.flrc_photo_stream_id, total - 1U,
                                    total, frame.flrc_photo_payload, nullptr,
                                    0U, dims,
                                    MESHTASTIC_FLRC_MEDIA_KIND_PHOTO_JPEG);
        state = mesh_flrc_voice_fast_transmit(lr2021, packet, sizeof(packet));
        if(state != RADIOLIB_ERR_NONE) {
            ok = false;
            daemon_event("FLRC photo TX done repeat=%u failed state=%d %s",
                         r + 1U, state, error_name(state));
            break;
        }
        usleep(MESHTASTIC_FLRC_PHOTO_PACKET_GAP_US);
    }
    if(ok) {
        uint64_t elapsed_ms = (monotonic_us() - start_us) / 1000ULL;
        mesh_photo_tx_stream_count++;
        mesh_photo_tx_chunk_count +=
            (uint64_t)total * mesh_media_cfg.photo_data_repeat;
        if(!daemon_chat_update_photo_stream_status(frame.flrc_photo_stream_id,
                                                   "sent",
                                                   (unsigned long)elapsed_ms)) {
            daemon_chat("TX 0x%08x photo %ux%u jpg packets=%u repeat=%u stream=0x%08x state=sent elapsed=%lums sha256=%s",
                        frame.from_node, frame.flrc_photo_width,
                        frame.flrc_photo_height, total,
                        (unsigned)mesh_media_cfg.photo_data_repeat,
                        frame.flrc_photo_stream_id,
                        (unsigned long)elapsed_ms,
                        stream_sha256.c_str());
        }
        daemon_event("FLRC photo TX done stream=0x%08x packets=%u repeat=%u elapsed_ms=%lu sha256=%s",
                     frame.flrc_photo_stream_id, total,
                     MESHTASTIC_FLRC_PHOTO_DATA_REPEAT,
                     (unsigned long)elapsed_ms, stream_sha256.c_str());
    } else {
        if(!daemon_chat_update_photo_stream_status(frame.flrc_photo_stream_id,
                                                   "failed", 0UL)) {
            daemon_chat("TX 0x%08x photo FLRC failed stream=0x%08x",
                        frame.from_node, frame.flrc_photo_stream_id);
        }
    }
    (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021, profile);
    return true;
}

static bool mesh_flrc_photo_rx_session(const probe_options_t &opts,
                                       PhysicalLayer *radio,
                                       chip_type_t chip, SX1262 *sx1262,
                                       LR2021 *lr2021,
                                       const probe_profile_t *profile,
                                       const mesh_header_t &mesh_header,
                                       const mesh_flrc_voice_header_t &invite,
                                       float control_rssi,
                                       const std::string *channel_psk)
{
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> received;
    std::vector<uint8_t> stream;
    uint64_t timeout_us;
    uint16_t received_count = 0U;
    uint16_t width = (uint16_t)(invite.duration_ms >> 16U);
    uint16_t height = (uint16_t)(invite.duration_ms & 0xffffU);
    int16_t state;
    bool repair_attempted = false;

    if(chip != CHIP_LR2021 || !lr2021) {
        daemon_event("FLRC photo invite ignored from=0x%08x reason=not-lr2021 chip=%s",
                     mesh_header.from, chip_name(chip));
        return false;
    }
    if(invite.total == 0U ||
       invite.total > MESHTASTIC_FLRC_PHOTO_MAX_PACKETS ||
       invite.total_size == 0U ||
       invite.total_size > MESHTASTIC_FLRC_PHOTO_MAX_BYTES ||
       invite.total_size >
       invite.total * MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN) {
        daemon_event("FLRC photo invite invalid from=0x%08x stream=0x%08x packets=%u size=%u",
                     mesh_header.from, invite.stream_id, invite.total,
                     invite.total_size);
        return false;
    }
    if(width == 0U || height == 0U) {
        width = MESHTASTIC_FLRC_PHOTO_MAX_W;
        height = MESHTASTIC_FLRC_PHOTO_MAX_H;
    }
    mesh_media_config_clamp();
    active_op = OP_IDLE;
    state = mesh_flrc_voice_begin(lr2021);
    if(state != RADIOLIB_ERR_NONE) {
        daemon_chat("RX 0x%08x photo FLRC init failed: %s",
                    mesh_header.from, error_name(state));
        (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021,
                                           profile);
        return true;
    }
    chunks.assign(invite.total, std::vector<uint8_t>());
    received.assign(invite.total, 0U);
    timeout_us = (uint64_t)invite.total * 9000ULL + 4000000ULL;
    if(timeout_us < 7000000ULL) {
        timeout_us = 7000000ULL;
    }
    if(timeout_us > 30000000ULL) {
        timeout_us = 30000000ULL;
    }
    daemon_event("FLRC photo RX window from=0x%08x stream=0x%08x packets=%u bytes=%u size=%ux%u timeout_ms=%lu control_rssi=%.1f",
                 mesh_header.from, invite.stream_id, invite.total,
                 invite.total_size, width, height,
                 (unsigned long)(timeout_us / 1000ULL), control_rssi);
    (void)mesh_flrc_photo_collect_window(
        lr2021, invite, &chunks, &received, &received_count, timeout_us,
        "RX", false);
    (void)lr2021->standby();
    for(uint8_t repair_round = 1U;
        received_count < invite.total &&
        repair_round <= mesh_media_cfg.photo_repair_rounds;
        repair_round++) {
        std::vector<uint8_t> repair_payload;
        tx_frame_t repair_frame;
        uint16_t missing =
            mesh_flrc_photo_missing_count(received, invite.total);
        uint16_t before = received_count;

        repair_attempted = true;
        (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021,
                                           profile);
        repair_payload = mesh_flrc_photo_make_repair_request_payload(
            invite, received, received_count, repair_round);
        if(!build_mesh_direct_data_frame(
               opts, mesh_header.from, mesh_header.channel,
               MESHTASTIC_PRIVATE_APP, repair_payload, false, 0U, 0U, 0U,
               false, "photo-repair-req", channel_psk, &repair_frame)) {
            mesh_photo_repair_fail_count++;
            daemon_event("FLRC photo repair request build failed to=0x%08x stream=0x%08x missing=%u round=%u",
                         mesh_header.from, invite.stream_id, missing,
                         repair_round);
            break;
        }
        daemon_event("FLRC photo repair request TX to=0x%08x stream=0x%08x missing=%u received=%u/%u round=%u",
                     mesh_header.from, invite.stream_id, missing,
                     received_count, invite.total, repair_round);
        if(!mesh_lora_sync_transmit(radio, repair_frame,
                                    "FLRC photo repair request")) {
            mesh_photo_repair_fail_count++;
            break;
        }
        mesh_photo_repair_req_tx_count++;
        state = mesh_flrc_voice_begin(lr2021);
        if(state != RADIOLIB_ERR_NONE) {
            mesh_photo_repair_fail_count++;
            daemon_event("FLRC photo repair RX init failed state=%d %s",
                         state, error_name(state));
            break;
        }
        (void)mesh_flrc_photo_collect_window(
            lr2021, invite, &chunks, &received, &received_count,
            (uint64_t)mesh_media_cfg.photo_repair_window_ms * 1000ULL,
            "repair RX", true);
        (void)lr2021->standby();
        daemon_event("FLRC photo repair round done stream=0x%08x round=%u added=%u received=%u/%u",
                     invite.stream_id, repair_round,
                     (unsigned)(received_count - before), received_count,
                     invite.total);
    }
    if(received_count == invite.total) {
        char path[192];
        char errbuf[128];
        std::string stream_sha256;

        stream.reserve(invite.total_size);
        for(uint16_t seq = 0U; seq < invite.total; seq++) {
            stream.insert(stream.end(), chunks[seq].begin(), chunks[seq].end());
        }
        if(stream.size() > invite.total_size) {
            stream.resize(invite.total_size);
        }
        if((uint32_t)stream.size() == invite.total_size &&
           crc32_update(0, stream.data(), stream.size()) == invite.stream_crc) {
            stream_sha256 = mesh_sha256_hex_vector(stream);
            if(mesh_photo_store_dir_ensure()) {
                snprintf(path, sizeof(path),
                         MESHTASTIC_PHOTO_STORE_DIR
                         "/RX_%08x_%08x.jpg",
                         mesh_header.from, invite.stream_id);
                if(mesh_photo_write_file(path, stream, errbuf,
                                         sizeof(errbuf))) {
                    mesh_photo_rx_complete_count++;
                    daemon_chat("RX 0x%08x photo %ux%u jpg packets=%u rssi=%ddBm stream=0x%08x sha256=%s file=%s",
                                mesh_header.from, width, height,
                                invite.total, (int)roundf(control_rssi),
                                invite.stream_id, stream_sha256.c_str(), path);
                    daemon_event("FLRC photo RX saved from=0x%08x stream=0x%08x bytes=%u packets=%u sha256=%s file=%s",
                                 mesh_header.from, invite.stream_id,
                                 (unsigned)stream.size(), invite.total,
                                 stream_sha256.c_str(), path);
                    if(repair_attempted) {
                        mesh_photo_repair_complete_count++;
                    }
                } else {
                    mesh_photo_rx_decode_fail_count++;
                    daemon_chat("RX 0x%08x FLRC photo write failed: %s",
                                mesh_header.from, errbuf);
                }
            } else {
                mesh_photo_rx_decode_fail_count++;
                daemon_chat("RX 0x%08x FLRC photo store unavailable",
                            mesh_header.from);
            }
        } else {
            mesh_photo_rx_decode_fail_count++;
            daemon_chat("RX 0x%08x FLRC photo crc failed packets=%u/%u",
                        mesh_header.from, received_count, invite.total);
        }
    } else {
        mesh_photo_rx_decode_fail_count++;
        mesh_photo_repair_fail_count++;
        daemon_chat("RX 0x%08x FLRC photo incomplete packets=%u/%u",
                    mesh_header.from, received_count, invite.total);
    }
    (void)mesh_flrc_voice_restore_lora(chip, radio, sx1262, lr2021, profile);
    return true;
}

static uint32_t mesh_voice_estimate_chunks_airtime_ms(
    PhysicalLayer *radio, const probe_options_t &opts, uint32_t portnum,
    const std::vector<std::vector<uint8_t>> &chunks)
{
    uint64_t total_ms = 0ULL;
    probe_options_t estimate_opts = opts;

    if(!radio || chunks.empty()) {
        return 0U;
    }
    estimate_opts.packet_id = 1U;
    for(size_t i = 0; i < chunks.size(); i++) {
        tx_frame_t frame;

        if(!build_tx_data_frame(estimate_opts, portnum, chunks[i], 0U,
                                "voice-estimate", &frame)) {
            continue;
        }
        total_ms += mesh_radio_airtime_ms(radio, frame.bytes.size());
        if(total_ms > UINT32_MAX) {
            return UINT32_MAX;
        }
    }
    return (uint32_t)total_ms;
}

static bool mesh_decode_payload_for_known_channel(
    const probe_options_t &opts, const mesh_header_t &header,
    const std::vector<uint8_t> &encrypted_payload,
    std::vector<uint8_t> *decoded_payload, mesh_data_proto_t *decoded,
    mesh_channel_match_t *match)
{
    for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS; i++) {
        std::vector<uint8_t> key;
        std::vector<uint8_t> candidate;
        mesh_data_proto_t candidate_decoded;
        std::string name;
        std::string psk;
        uint32_t role = mesh_channel_slot_role(opts, i);
        uint8_t hash;

        if(role == MESHTASTIC_CHANNEL_ROLE_DISABLED) {
            continue;
        }
        name = mesh_channel_slot_name(opts, i);
        psk = mesh_channel_slot_psk(opts, i);
        if(!parse_psk(psk, &key)) {
            continue;
        }
        hash = mesh_channel_hash(name, key);
        if(header.channel != hash) {
            continue;
        }
        candidate = encrypted_payload;
        if(!aes_ctr_crypt(key, header.from, header.id, &candidate) ||
           !decode_data_proto(candidate.data(), candidate.size(),
                              &candidate_decoded)) {
            continue;
        }
        if(decoded_payload) {
            *decoded_payload = candidate;
        }
        if(decoded) {
            *decoded = candidate_decoded;
        }
        if(match) {
            match->valid = true;
            match->index = i;
            match->role = role;
            match->hash = hash;
            match->name = name;
            match->psk = psk;
        }
        return true;
    }
    return false;
}

static bool mesh_decode_payload_for_pki(
    const probe_options_t &opts, const mesh_header_t &header,
    const std::vector<uint8_t> &encrypted_payload,
    std::vector<uint8_t> *decoded_payload, mesh_data_proto_t *decoded)
{
    uint8_t remote_public[MESHTASTIC_CURVE25519_KEY_LEN];
    std::vector<uint8_t> candidate;
    mesh_data_proto_t candidate_decoded;

    if(header.channel != 0U || header.to != opts.from_node ||
       header.from == 0U || header.from == opts.from_node ||
       meshtastic_node_is_broadcast(header.to)) {
        return false;
    }
    if(!mesh_pki_identity.ready) {
        daemon_event("PKI decrypt skipped from=0x%08x reason=no-local-identity",
                     header.from);
        return false;
    }
    if(!mesh_node_copy_public_key(header.from, remote_public)) {
        daemon_event("PKI decrypt skipped from=0x%08x reason=no-remote-key",
                     header.from);
        return false;
    }
    if(!mesh_pki_decrypt_payload(remote_public, header.from, header.id,
                                 encrypted_payload, &candidate) ||
       !decode_data_proto(candidate.data(), candidate.size(),
                          &candidate_decoded)) {
        daemon_event("PKI decrypt failed from=0x%08x id=0x%08x len=%u",
                     header.from, header.id,
                     (unsigned)encrypted_payload.size());
        return false;
    }
    if(decoded_payload) {
        *decoded_payload = candidate;
    }
    if(decoded) {
        *decoded = candidate_decoded;
    }
    return true;
}

static bool process_mesh_rx(const probe_options_t &opts, PhysicalLayer *radio,
                            chip_type_t chip, SX1262 *sx1262,
                            LR2021 *lr2021,
                            const probe_profile_t *profile,
                            const uint8_t *data, size_t len, float rssi,
                            float snr, tx_frame_t *rebroadcast_frame)
{
    mesh_header_t header;
    mesh_channel_match_t channel_info;
    std::vector<uint8_t> encrypted_payload;
    std::vector<uint8_t> payload;
    mesh_data_proto_t decoded;
    uint8_t hop_limit;
    uint8_t hop_start;
    bool channel_match = false;
    bool pki_match = false;
    bool secure_match = false;
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

    encrypted_payload.assign(data + MESHTASTIC_HEADER_LENGTH, data + len);
    data_ok = mesh_decode_payload_for_known_channel(
        opts, header, encrypted_payload, &payload, &decoded, &channel_info);
    channel_match = data_ok && channel_info.valid;
    if(!data_ok) {
        pki_match = mesh_decode_payload_for_pki(opts, header,
                                                encrypted_payload,
                                                &payload, &decoded);
        data_ok = pki_match;
    }
    secure_match = channel_match || pki_match;
    if(data_ok && duplicate && channel_match &&
       header.from == opts.from_node &&
       header.to == MESHTASTIC_NODENUM_BROADCAST &&
       mesh_header_want_ack(header)) {
        (void)mesh_ack_complete_implicit(opts.from_node, header);
    }
    if(data_ok) {
        if(pki_match) {
            daemon_event("RX PKI match from=0x%08x id=0x%08x port=%u",
                         header.from, header.id, decoded.portnum);
        } else {
            daemon_event("RX channel match index=%u role=%u name=%s hash=0x%02x",
                         channel_info.index, channel_info.role,
                         channel_info.name.empty() ? "<empty>" :
                         channel_info.name.c_str(), channel_info.hash);
        }
        if(channel_match && header.from != opts.from_node) {
            if(mesh_node_update_link_info(header.from, channel_info.index,
                                          hop_start, hop_limit)) {
                phoneapi_notify_node_update(header.from, "link");
            }
        }
        if(secure_match && !duplicate && header.from != opts.from_node) {
            uint32_t phoneapi_channel =
                (pki_match ||
                 channel_info.role == MESHTASTIC_CHANNEL_ROLE_PRIMARY) ?
                0U : channel_info.index;

            phoneapi_notify_mesh_rx(header, payload, phoneapi_channel,
                                    rssi, snr);
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
            if(!duplicate && secure_match && !clean.empty() &&
               (header.to == MESHTASTIC_NODENUM_BROADCAST ||
                header.to == opts.from_node || header.from == opts.from_node)) {
                if(mesh_chat_text_seen_recently(header.from, header.to, clean)) {
                    daemon_event("Mesh chat duplicate text suppressed from=0x%08x to=0x%08x text=%s",
                                 header.from, header.to, clean.c_str());
                } else {
                    daemon_chat("RX 0x%08x mode=%s to=0x%08x ch=%u rssi=%ddBm snr=%.1f: %s",
                                header.from,
                                meshtastic_node_is_broadcast(header.to) ?
                                "channel" : "direct",
                                header.to, channel_info.index,
                                (int)roundf(rssi), snr, clean.c_str());
                }
            }
        } else if(decoded.portnum == MESHTASTIC_TEXT_MESSAGE_COMPRESSED_APP) {
            size_t preview_len = decoded.payload.size();
            std::string preview;

            if(preview_len > 24U) {
                preview_len = 24U;
            }
            preview = mesh_hex_encode_bytes(decoded.payload.data(), preview_len);
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u ack=%s rssi=%.1f snr=%.1f port=%u compressed_text_len=%u preview=%s%s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         mesh_header_want_ack(header) ? "yes" : "no",
                         rssi, snr, decoded.portnum,
                         (unsigned)decoded.payload.size(),
                         preview.empty() ? "-" : preview.c_str(),
                         decoded.payload.size() > preview_len ? "..." : "",
                         duplicate ? " duplicate" : "");
            if(!duplicate && secure_match &&
               (header.to == MESHTASTIC_NODENUM_BROADCAST ||
                header.to == opts.from_node || header.from == opts.from_node)) {
                daemon_chat("RX 0x%08x compressed text len=%u unsupported: unishox2 decoder required",
                            header.from, (unsigned)decoded.payload.size());
            }
        } else if(decoded.portnum == MESHTASTIC_AUDIO_APP) {
            bool codec2_header = mesh_voice_payload_is_codec2(decoded.payload);
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u ack=%s rssi=%.1f snr=%.1f port=%u codec2=%s len=%u%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         mesh_header_want_ack(header) ? "yes" : "no",
                         rssi, snr, decoded.portnum,
                         codec2_header ? "yes" : "no",
                         (unsigned)decoded.payload.size(),
                         duplicate ? " duplicate" : "");
            if(!duplicate && secure_match && header.from != opts.from_node) {
                if(codec2_header) {
                    char path[128];
                    char errbuf[128];
                    unsigned duration_ms = 0U;

                    mesh_voice_rx_chunk_count++;
                    snprintf(path, sizeof(path),
                             "/tmp/k230_mesh_voice_rx_%08x_%08x.raw",
                             header.from, header.id);
                    if(mesh_voice_decode_codec2_payload_to_file(
                           decoded.payload, path, &duration_ms, errbuf,
                           sizeof(errbuf))) {
                        mesh_voice_rx_complete_count++;
                        if(rssi > -200.0f && rssi < 20.0f) {
                            daemon_chat("RX 0x%08x voice %.1fs codec=codec2 rssi=%ddBm file=%s",
                                        header.from,
                                        (double)duration_ms / 1000.0,
                                        (int)roundf(rssi), path);
                        } else {
                            daemon_chat("RX 0x%08x voice %.1fs codec=codec2 rssi=-- file=%s",
                                        header.from,
                                        (double)duration_ms / 1000.0,
                                        path);
                        }
                    } else {
                        mesh_voice_rx_decode_fail_count++;
                        daemon_chat("RX 0x%08x codec2 voice decode failed: %s",
                                    header.from, errbuf);
                    }
                } else {
                    mesh_voice_rx_decode_fail_count++;
                    daemon_chat("RX 0x%08x codec2 voice invalid-header len=%u",
                                header.from,
                                (unsigned)decoded.payload.size());
                }
            }
        } else if(decoded.portnum == MESHTASTIC_PRIVATE_APP) {
            mesh_flrc_photo_repair_request_t repair_request;

            if(mesh_flrc_photo_parse_repair_request_payload(
                   decoded.payload, &repair_request)) {
                daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u photo_repair_req stream=0x%08x missing=%u received=%u/%u round=%u%s",
                             (unsigned long)rx_count, header.from, header.to,
                             header.id, header.channel, hop_limit, hop_start,
                             rssi, snr, decoded.portnum,
                             repair_request.stream_id,
                             repair_request.missing_count,
                             repair_request.received_count,
                             repair_request.total, repair_request.round,
                             duplicate ? " duplicate" : "");
                if(!duplicate && secure_match &&
                   header.to == opts.from_node &&
                   header.from != opts.from_node) {
                    (void)mesh_flrc_photo_repair_tx_session(
                        radio, chip, sx1262, lr2021, profile, header.from,
                        repair_request);
                    return false;
                }
            } else if(mesh_flrc_photo_payload_is_invite(decoded.payload,
                                                        nullptr)) {
                mesh_flrc_voice_header_t invite;

                if(mesh_flrc_photo_payload_is_invite(decoded.payload,
                                                     &invite)) {
                    uint16_t width = (uint16_t)(invite.duration_ms >> 16U);
                    uint16_t height =
                        (uint16_t)(invite.duration_ms & 0xffffU);

                    daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u flrc_photo_invite stream=0x%08x packets=%u bytes=%u size=%ux%u%s",
                                 (unsigned long)rx_count, header.from,
                                 header.to, header.id, header.channel,
                                 hop_limit, hop_start, rssi, snr,
                                 decoded.portnum, invite.stream_id,
                                 invite.total, invite.total_size, width,
                                 height, duplicate ? " duplicate" : "");
                    if(!duplicate && secure_match &&
                       header.from != opts.from_node) {
                        const std::string *photo_psk =
                            channel_match ? &channel_info.psk : nullptr;

                        (void)mesh_flrc_photo_rx_session(
                            opts, radio, chip, sx1262, lr2021, profile,
                            header, invite, rssi, photo_psk);
                    }
                }
            } else if(mesh_flrc_voice_payload_is_invite(decoded.payload,
                                                        nullptr)) {
                mesh_flrc_voice_header_t invite;

                if(mesh_flrc_voice_payload_is_invite(decoded.payload,
                                                     &invite)) {
                    daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u flrc_voice_invite stream=0x%08x packets=%u bytes=%u%s",
                                 (unsigned long)rx_count, header.from,
                                 header.to, header.id, header.channel,
                                 hop_limit, hop_start, rssi, snr,
                                 decoded.portnum, invite.stream_id,
                                 invite.total, invite.total_size,
                                 duplicate ? " duplicate" : "");
                    if(!duplicate && secure_match &&
                       header.from != opts.from_node) {
                        (void)mesh_flrc_voice_rx_session(
                            opts, radio, chip, sx1262, lr2021, profile,
                            header, invite, rssi);
                    }
                }
            } else if(mesh_voice_payload_is_k230(decoded.payload)) {
                (void)mesh_voice_handle_rx(opts, header, decoded.payload,
                                           rssi, snr, duplicate,
                                           secure_match);
            } else {
                daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u payload_len=%u%s",
                             (unsigned long)rx_count, header.from, header.to,
                             header.id, header.channel, hop_limit, hop_start,
                             rssi, snr, decoded.portnum,
                             (unsigned)decoded.payload.size(),
                             duplicate ? " duplicate" : "");
            }
        } else if(decoded.portnum == MESHTASTIC_POSITION_APP) {
            mesh_position_info_t position;
            bool position_request = decoded.want_response &&
                                    decoded.payload.empty();
            bool position_ok = !position_request &&
                               decode_position_proto(decoded.payload,
                                                     &position);
            char alt_text[24];
            char speed_text[24];
            char track_text[24];

            if(position_ok && secure_match) {
                mesh_node_update_position(header.from, position);
                mesh_remote_status_mark_replied(header.from,
                                                decoded.portnum,
                                                decoded.request_id,
                                                "position_received");
                phoneapi_notify_node_update(header.from, "position");
            }
            if(position_ok && position.has_altitude) {
                snprintf(alt_text, sizeof(alt_text), "%dm",
                         position.altitude_m);
            } else {
                snprintf(alt_text, sizeof(alt_text), "-");
            }
            if(position_ok && position.has_ground_speed) {
                snprintf(speed_text, sizeof(speed_text), "%um/s",
                         position.ground_speed_cms);
            } else {
                snprintf(speed_text, sizeof(speed_text), "-");
            }
            if(position_ok && position.has_ground_track) {
                snprintf(track_text, sizeof(track_text), "%.2fdeg",
                         position.ground_track_1e5 / 100.0);
            } else {
                snprintf(track_text, sizeof(track_text), "-");
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u pos=%s lat=%.7f lon=%.7f alt=%s speed=%s track=%s sats=%u precision=%u%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         position_request ? "request" :
                         (position_ok ? "ok" : "decode-failed"),
                         position.latitude_i * 1e-7,
                         position.longitude_i * 1e-7,
                         alt_text, speed_text, track_text,
                         position.sats_in_view, position.precision_bits,
                         duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_WAYPOINT_APP) {
            mesh_waypoint_info_t waypoint;
            bool waypoint_ok = decode_waypoint_proto(decoded.payload,
                                                     &waypoint);
            std::string clean_name = mesh_clean_text(waypoint.name);
            std::string clean_desc = mesh_clean_text(waypoint.description);

            if(waypoint_ok && secure_match && !duplicate) {
                mesh_waypoint_update(header.from, waypoint);
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u waypoint=%s wp_id=0x%08x lat=%.7f lon=%.7f expire=%u locked=0x%08x icon=0x%08x name=%s desc=%s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         waypoint_ok ? "ok" : "decode-failed",
                         waypoint.id, waypoint.latitude_i * 1e-7,
                         waypoint.longitude_i * 1e-7, waypoint.expire,
                         waypoint.locked_to, waypoint.icon,
                         clean_name.empty() ? "-" : clean_name.c_str(),
                         clean_desc.empty() ? "-" : clean_desc.c_str(),
                         duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_NODEINFO_APP) {
            mesh_user_info_t user;
            bool user_ok = decode_user_proto(decoded.payload, &user);
            std::string clean_long = mesh_clean_text(user.long_name);
            std::string clean_short = mesh_clean_text(user.short_name);

            if(secure_match && user_ok) {
                mesh_node_update_user(header.from, user);
                mesh_remote_status_mark_replied(header.from,
                                                decoded.portnum,
                                                decoded.request_id,
                                                "nodeinfo_received");
                phoneapi_notify_node_update(header.from, "user");
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
            bool telemetry_request = decoded.want_response && !telemetry_ok;
            std::string summary = telemetry_summary(telemetry);

            if(telemetry_ok && secure_match) {
                mesh_node_update_telemetry(header.from, telemetry);
                mesh_remote_status_mark_replied(header.from,
                                                decoded.portnum,
                                                decoded.request_id,
                                                telemetry.has_environment_metrics ?
                                                "environment_telemetry_received" :
                                                "device_telemetry_received");
                phoneapi_notify_node_update(header.from, "telemetry");
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u telemetry=%s %s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         telemetry_request ? "request" :
                         (telemetry_ok ? "ok" : "decode-failed"),
                         summary.c_str(), duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_TRACEROUTE_APP) {
            std::string route_summary;
            bool route_ok = decode_route_discovery_proto(decoded.payload,
                                                         &route_summary);

            if(route_ok && secure_match) {
                mesh_node_update_route_info(header.from, route_summary);
                mesh_remote_status_mark_replied(header.from,
                                                decoded.portnum,
                                                decoded.request_id,
                                                "traceroute_received");
                phoneapi_notify_node_update(header.from, "traceroute");
            }
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u traceroute=%s request=0x%08x reply=0x%08x %s%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum,
                         route_ok ? "ok" : "empty-or-decode-failed",
                         decoded.request_id, decoded.reply_id,
                         route_summary.empty() ? "-" :
                             route_summary.c_str(),
                         duplicate ? " duplicate" : "");
        } else if(decoded.portnum == MESHTASTIC_NEIGHBORINFO_APP) {
            mesh_neighbor_info_t neighbor_info;
            bool neighbor_ok = decode_neighbor_info_proto(decoded.payload,
                                                          &neighbor_info);

            if(neighbor_ok && secure_match) {
                mesh_node_update_neighbor_info(header.from, neighbor_info);
                mesh_remote_status_mark_replied(header.from,
                                                decoded.portnum,
                                                decoded.request_id,
                                                "neighborinfo_received");
                phoneapi_notify_node_update(header.from, "neighbor");
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
            daemon_event("RX %lu mesh from=0x%08x to=0x%08x id=0x%08x ch=0x%02x hop=%u/%u rssi=%.1f snr=%.1f port=%u request=0x%08x routing=%s err=%u(%s)%s",
                         (unsigned long)rx_count, header.from, header.to,
                         header.id, header.channel, hop_limit, hop_start,
                         rssi, snr, decoded.portnum, decoded.request_id,
                         routing_ok ? "ok" : "decode-failed",
                         error_reason, mesh_routing_error_name(error_reason),
                         duplicate ? " duplicate" : "");
            if(secure_match && routing_ok && header.to == opts.from_node &&
               decoded.request_id != 0U) {
                if(!mesh_ack_complete(header.from, decoded.request_id,
                                      error_reason)) {
                    daemon_event("Mesh ACK no pending id=0x%08x from=0x%08x err=%u(%s)",
                                 decoded.request_id, header.from,
                                 error_reason,
                                 mesh_routing_error_name(error_reason));
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
                     snr, (unsigned)encrypted_payload.size(),
                     duplicate ? " duplicate" : "");
    }

    if(secure_match && data_ok && decoded.want_response && !duplicate &&
       header.to == opts.from_node && header.from != opts.from_node) {
        if(build_mesh_want_response_frame(opts, header, decoded, snr,
                                          pki_match ? nullptr :
                                          &channel_info.psk,
                                          pki_match,
                                          rebroadcast_frame)) {
            daemon_event("WantResponse queued port=%u request=0x%08x to=0x%08x",
                         decoded.portnum, header.id, header.from);
            return true;
        }
        daemon_event("WantResponse unsupported/skipped port=%u request=0x%08x from=0x%08x",
                     decoded.portnum, header.id, header.from);
    }

    ack_candidate = secure_match && data_ok &&
                    header.to == opts.from_node &&
                    header.from != opts.from_node &&
                    mesh_header_want_ack(header) &&
                    (decoded.portnum != MESHTASTIC_ROUTING_APP ||
                     decoded.request_id != 0U);
    if(ack_candidate) {
        ack_wants_ack = false;
    }
    if(ack_candidate) {
        mesh_header_t ack_source_header = header;
        std::string primary_psk = opts.psk;
        std::vector<uint8_t> primary_key;

        if(pki_match && parse_psk(primary_psk, &primary_key)) {
            ack_source_header.channel = mesh_channel_hash(
                effective_mesh_channel_name(opts), primary_key);
        }
        if(!build_mesh_ack_frame(opts, ack_source_header,
                                 MESHTASTIC_ROUTING_ERROR_NONE,
                                 ack_wants_ack,
                                 pki_match ? &primary_psk :
                                 &channel_info.psk,
                                 rebroadcast_frame)) {
            ack_candidate = false;
        }
    }
    if(ack_candidate) {
        daemon_event("Mesh ACK candidate req=0x%08x to=0x%08x ack=%s",
                     header.id, header.from,
                     ack_wants_ack ? "reliable" : "plain");
        if(duplicate) {
            daemon_event("Mesh ACK duplicate request id=0x%08x from=0x%08x",
                         header.id, header.from);
            return true;
        }
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
                                          size_t pending_send_count)
{
    char buf[3800];
    char ble_detail[160];
    char ble_pair[16];
    char slot_text[16];
    char gps_detail[160];
    char gnss_phase[24];
    char photo_drop_seq[256];
    phoneapi_bridge_state_t ble_state;
    const char *queued = pending_send_count > 0U ? "1" : "0";
    uint64_t now = monotonic_us();
    uint64_t last_nmea_ms = 0ULL;
    std::string channel_url = meshtastic_channel_url(opts);
    std::string pki_key = "-";
    size_t node_public_key_count = 0;
    float channel_util = mesh_airtime_channel_util_percent();
    float air_tx = mesh_airtime_tx_util_percent();
    float duty_cycle = meshtastic_region_duty_cycle_percent(opts);
    const char *voice_mode = chip == CHIP_LR2021 ? "flrc" : "disabled";
    const char *photo_mode = chip == CHIP_LR2021 ? "flrc" : "disabled";

    mesh_photo_debug_drop_seq_text(photo_drop_seq, sizeof(photo_drop_seq));
    if(opts.position_enabled &&
       (!mesh_gnss.has_fix || strcmp(mesh_gnss.gps_state, "fix") != 0)) {
        (void)nrf9151_gnss_apply_cache_fix(true);
    }
    if(opts.phoneapi_enabled) {
        ble_state = phoneapi_bridge_get_state(ble_detail, sizeof(ble_detail));
    } else {
        ble_state = PHONEAPI_BRIDGE_OFFLINE;
        snprintf(ble_detail, sizeof(ble_detail), "%s", "disabled");
    }
    if(mesh_pki_public_key_available()) {
        pki_key = mesh_hex_encode_bytes(mesh_pki_identity.public_key, 4U);
    }
    for(size_t i = 0; i < mesh_node_count; i++) {
        if(mesh_nodes[i].node != 0U && mesh_nodes[i].has_public_key) {
            node_public_key_count++;
        }
    }
    if(!phoneapi_bridge_get_pairing_code(ble_pair, sizeof(ble_pair))) {
        snprintf(ble_pair, sizeof(ble_pair), "%s", "-");
    }
    if(opts.frequency_slot > 0U) {
        snprintf(slot_text, sizeof(slot_text), "%u", opts.frequency_slot);
    } else {
        snprintf(slot_text, sizeof(slot_text), "%s", "auto");
    }
    snprintf(gps_detail, sizeof(gps_detail), "%s", mesh_gnss.detail);
    for(size_t i = 0; gps_detail[i]; i++) {
        if(isspace((unsigned char)gps_detail[i])) {
            gps_detail[i] = '_';
        }
    }
    snprintf(gnss_phase, sizeof(gnss_phase), "%s", nrf9151_gnss_phase());
    if(mesh_gnss.last_nmea_us > 0ULL && now >= mesh_gnss.last_nmea_us) {
        last_nmea_ms = (now - mesh_gnss.last_nmea_us) / 1000ULL;
    }
    snprintf(buf, sizeof(buf),
             "OK version=%s chip=%s op=%s tx=%lu rx=%lu queued=%s queued_count=%u "
             "ble=%s ble_detail=%s ble_pair=%s "
             "pki=%s pki_generated=%s pki_key=%s node_keys=%u "
             "hist=%u dup=%lu rebroadcast=%lu rebroadcast_drop=%lu "
             "delayed=%u next_rebroadcast_ms=%u "
             "ack_pending=%u ack_next_ms=%u ack_rx=%lu nak_rx=%lu "
             "ack_retry=%lu ack_timeout=%lu ack_drop=%lu "
             "voice=%s voice_freq=%.1f voice_br=%u voice_power=%d "
             "voice_tx_streams=%lu voice_tx_chunks=%lu voice_rx_chunks=%lu "
             "voice_rx_complete=%lu voice_rx_decode_fail=%lu "
             "photo=%s photo_tx_streams=%lu photo_tx_chunks=%lu "
             "photo_rx_chunks=%lu photo_rx_complete=%lu photo_rx_decode_fail=%lu "
             "photo_repair_req_tx=%lu photo_repair_req_rx=%lu "
             "photo_repair_tx_chunks=%lu photo_repair_rx_chunks=%lu "
             "photo_repair_complete=%lu photo_repair_fail=%lu "
             "photo_repeat=%u photo_repair_rounds=%u "
             "photo_repair_repeat=%u photo_repair_window_ms=%u "
             "photo_cache_ttl_sec=%u "
             "photo_drop=%s photo_drop_seq=%s photo_drop_rate=%u photo_drop_hits=%lu "
             "ch_util=%.1f air_tx=%.2f duty=%.1f air_tx_ms=%lu air_rx_ms=%lu "
             "nodeinfo_tx=%lu nodeinfo_drop=%lu next_nodeinfo_ms=%u "
             "position=%s fixed=%s nrf9151=%s gps=%s gnss_phase=%s gps_detail=%s "
             "nmea_rx=%lu nmea_valid=%lu nmea_nofix=%lu last_nmea_ms=%lu "
             "gnss_sats_seen=%u ttff_ms=%lu ttff_valid=%s "
             "position_tx=%lu position_drop=%lu next_position_ms=%u "
             "lat=%.7f lon=%.7f sats=%u fixed_lat=%.7f fixed_lon=%.7f "
             "telemetry=%s telemetry_env=%s telemetry_tx=%lu telemetry_drop=%lu next_telemetry_ms=%u "
             "nodedb=%u nodedb_load=%u nodedb_save=%u nodedb_fail=%u nodedb_dirty=%s "
             "region=%s preset=%s slot=%s resolved_slot=%u slots=%u freq=%.3f bw=%.1f sf=%u cr=4/%u sw=0x%02x manual_power=%s power=%d node=%s "
             "from=0x%08x to=0x%08x want_ack=%s relay=%s channel=%s channel_url=%s socket=%s\n",
             PROBE_VERSION, chip_name(chip), op_name(active_op),
             (unsigned long)tx_count, (unsigned long)rx_count, queued,
             (unsigned)pending_send_count,
             phoneapi_bridge_state_name(ble_state), ble_detail, ble_pair,
             mesh_pki_public_key_available() ? "ready" : "off",
             mesh_pki_identity.generated ? "yes" : "no",
             pki_key.c_str(), (unsigned)node_public_key_count,
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
             voice_mode, MESHTASTIC_FLRC_VOICE_FREQ_MHZ,
             MESHTASTIC_FLRC_VOICE_BR_KBPS,
             MESHTASTIC_FLRC_VOICE_POWER_DBM,
             (unsigned long)mesh_voice_tx_stream_count,
             (unsigned long)mesh_voice_tx_chunk_count,
             (unsigned long)mesh_voice_rx_chunk_count,
             (unsigned long)mesh_voice_rx_complete_count,
             (unsigned long)mesh_voice_rx_decode_fail_count,
             photo_mode,
             (unsigned long)mesh_photo_tx_stream_count,
             (unsigned long)mesh_photo_tx_chunk_count,
             (unsigned long)mesh_photo_rx_chunk_count,
             (unsigned long)mesh_photo_rx_complete_count,
             (unsigned long)mesh_photo_rx_decode_fail_count,
             (unsigned long)mesh_photo_repair_req_tx_count,
             (unsigned long)mesh_photo_repair_req_rx_count,
             (unsigned long)mesh_photo_repair_tx_chunk_count,
             (unsigned long)mesh_photo_repair_rx_chunk_count,
             (unsigned long)mesh_photo_repair_complete_count,
             (unsigned long)mesh_photo_repair_fail_count,
             (unsigned)mesh_media_cfg.photo_data_repeat,
             (unsigned)mesh_media_cfg.photo_repair_rounds,
             (unsigned)mesh_media_cfg.photo_repair_repeat,
             (unsigned)mesh_media_cfg.photo_repair_window_ms,
             (unsigned)mesh_media_cfg.photo_tx_cache_ttl_sec,
             mesh_photo_debug_drop.enabled ? "on" : "off",
             photo_drop_seq,
             (unsigned)mesh_photo_debug_drop.rate_percent,
             (unsigned long)mesh_photo_debug_drop.hit_count,
             channel_util, air_tx, duty_cycle,
             (unsigned long)mesh_airtime_tx_total_ms,
             (unsigned long)mesh_airtime_rx_total_ms,
             (unsigned long)mesh_nodeinfo_tx_count,
             (unsigned long)mesh_nodeinfo_drop_count,
             mesh_nodeinfo_next_ms(now),
             opts.position_enabled ? "on" : "off",
             opts.fixed_position_enabled ? "on" : "off",
             mesh_gnss.modem_state, mesh_gnss.gps_state, gnss_phase,
             gps_detail,
             (unsigned long)mesh_gnss.nmea_rx_count,
             (unsigned long)mesh_gnss.nmea_valid_count,
             (unsigned long)mesh_gnss.nmea_nofix_count,
             (unsigned long)last_nmea_ms,
             mesh_gnss.position.sats_in_view,
             (unsigned long)mesh_gnss.ttff_ms,
             mesh_gnss.ttff_valid ? "1" : "0",
             (unsigned long)mesh_position_tx_count,
             (unsigned long)mesh_position_drop_count,
             mesh_position_next_ms(now),
             mesh_gnss.has_fix ? mesh_gnss.position.latitude_i * 1e-7 : 0.0,
             mesh_gnss.has_fix ? mesh_gnss.position.longitude_i * 1e-7 : 0.0,
             mesh_gnss.has_fix ? mesh_gnss.position.sats_in_view : 0U,
             opts.fixed_position_enabled ?
             opts.fixed_position_latitude_i * 1e-7 : 0.0,
             opts.fixed_position_enabled ?
             opts.fixed_position_longitude_i * 1e-7 : 0.0,
             opts.telemetry_enabled ? "on" : "off",
             opts.environment_telemetry_enabled ? "on" : "off",
             (unsigned long)mesh_telemetry_tx_count,
             (unsigned long)mesh_telemetry_drop_count,
             mesh_telemetry_next_ms(now),
             (unsigned)mesh_node_count,
             mesh_nodedb_load_count,
             mesh_nodedb_save_count,
             mesh_nodedb_save_fail_count,
             mesh_nodedb_dirty ? "1" : "0",
             opts.resolved_region.empty() ? "-" : opts.resolved_region.c_str(),
             opts.resolved_preset.empty() ? "-" : opts.resolved_preset.c_str(),
             slot_text, opts.resolved_slot, opts.resolved_slot_count,
             opts.profile.freq, opts.profile.bandwidth, opts.profile.sf,
             opts.profile.cr, opts.profile.sync_word,
             opts.manual_power ? "true" : "false", opts.profile.power,
             opts.node_name.c_str(), opts.from_node,
             opts.to_node, opts.want_ack ? "on" : "off",
             opts.rebroadcast ? "on" : "off",
             effective_mesh_channel_name(opts).c_str(),
             channel_url.empty() ? "-" : channel_url.c_str(),
             opts.socket_path.c_str());
    return std::string(buf);
}

static std::string daemon_channel_url_response(const probe_options_t &opts)
{
    std::string url = meshtastic_channel_url(opts);

    if(url.empty()) {
        return "ERR channel-url\n";
    }
    return std::string("OK channel_url=") + url + "\n";
}

static const char *daemon_channel_role_name(uint32_t role)
{
    switch(role) {
    case MESHTASTIC_CHANNEL_ROLE_PRIMARY:
        return "primary";
    case MESHTASTIC_CHANNEL_ROLE_SECONDARY:
        return "secondary";
    case MESHTASTIC_CHANNEL_ROLE_DISABLED:
    default:
        return "disabled";
    }
}

static std::string daemon_channel_token(const std::string &text,
                                        const char *fallback)
{
    std::string out;

    for(char c : text) {
        if(out.size() >= 48U) {
            break;
        }
        if(c == '=' || isspace((unsigned char)c)) {
            out.push_back('_');
        } else if(isprint((unsigned char)c)) {
            out.push_back(c);
        }
    }
    if(out.empty()) {
        out = fallback ? fallback : "-";
    }
    return out;
}

static std::string daemon_channels_response(const probe_options_t &opts)
{
    char line[256];
    std::string response;

    snprintf(line, sizeof(line), "OK channels primary=%u\n",
             opts.primary_channel_index);
    response += line;
    for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS; i++) {
        uint32_t role = mesh_channel_slot_role(opts, i);
        std::string name = mesh_channel_slot_name(opts, i);
        std::string psk = mesh_channel_slot_psk(opts, i);
        std::vector<uint8_t> key;
        uint8_t hash = 0U;
        bool psk_ok = parse_psk(psk, &key);

        if(psk_ok) {
            hash = mesh_channel_hash(name, key);
        }
        snprintf(line, sizeof(line),
                 "CH index=%u primary=%s role=%s name=%s hash=0x%02x psk=%s uplink=%s downlink=%s muted=%s configured=%s\n",
                 i, i == opts.primary_channel_index ? "yes" : "no",
                 daemon_channel_role_name(role),
                 daemon_channel_token(name, "default").c_str(), hash,
                 psk_ok ? (psk == "default" ? "default" : "custom") :
                 "invalid",
                 opts.channels[i].uplink_enabled ? "yes" : "no",
                 opts.channels[i].downlink_enabled ? "yes" : "no",
                 opts.channels[i].is_muted ? "yes" : "no",
                 opts.channels[i].configured ? "yes" : "no");
        response += line;
    }
    return response;
}

static bool daemon_channel_role_from_token(const char *text, uint32_t *role)
{
    if(!text || !role) {
        return false;
    }
    if(strcasecmp(text, "primary") == 0 || strcmp(text, "1") == 0) {
        *role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
        return true;
    }
    if(strcasecmp(text, "secondary") == 0 || strcmp(text, "2") == 0) {
        *role = MESHTASTIC_CHANNEL_ROLE_SECONDARY;
        return true;
    }
    if(strcasecmp(text, "disabled") == 0 || strcasecmp(text, "off") == 0 ||
       strcmp(text, "0") == 0) {
        *role = MESHTASTIC_CHANNEL_ROLE_DISABLED;
        return true;
    }
    return false;
}

static std::string daemon_set_channel_slot_response(const std::string &line,
                                                    const probe_options_t &opts)
{
    probe_options_t updated = opts;
    char role_text[20];
    char name_text[80];
    char psk_text[100];
    unsigned index;
    uint32_t role;
    bool request_reconfigure = false;
    std::vector<uint8_t> key;

    if(sscanf(line.c_str(), "SET_CHANNEL_SLOT %u %19s %79s %99s",
              &index, role_text, name_text, psk_text) != 4 &&
       sscanf(line.c_str(), "set_channel_slot %u %19s %79s %99s",
              &index, role_text, name_text, psk_text) != 4) {
        return "ERR usage SET_CHANNEL_SLOT <0-7> <primary|secondary|disabled> <name|-> <psk>\n";
    }
    if(index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        return "ERR invalid-channel\n";
    }
    if(!daemon_channel_role_from_token(role_text, &role)) {
        return "ERR invalid-role\n";
    }
    if(role == MESHTASTIC_CHANNEL_ROLE_DISABLED &&
       index == updated.primary_channel_index) {
        return "ERR cannot-disable-primary\n";
    }
    if(role != MESHTASTIC_CHANNEL_ROLE_DISABLED &&
       !parse_psk(psk_text, &key)) {
        return "ERR invalid-psk\n";
    }
    if(role == MESHTASTIC_CHANNEL_ROLE_SECONDARY &&
       index == updated.primary_channel_index) {
        return "ERR primary-role-required\n";
    }

    updated.channels[index] = mesh_channel_slot_t();
    updated.channels[index].configured =
        role != MESHTASTIC_CHANNEL_ROLE_DISABLED;
    updated.channels[index].role = role;
    if(role != MESHTASTIC_CHANNEL_ROLE_DISABLED) {
        if(strcmp(name_text, "-") == 0 || strcasecmp(name_text, "default") == 0) {
            updated.channels[index].name.clear();
        } else {
            updated.channels[index].name = mesh_clean_text(name_text);
        }
        updated.channels[index].psk = psk_text;
    }

    if(role == MESHTASTIC_CHANNEL_ROLE_PRIMARY) {
        for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS; i++) {
            if(i != index && updated.channels[i].configured &&
               updated.channels[i].role == MESHTASTIC_CHANNEL_ROLE_PRIMARY) {
                updated.channels[i].role = MESHTASTIC_CHANNEL_ROLE_SECONDARY;
            }
        }
        updated.primary_channel_index = index;
        updated.channel_name = updated.channels[index].name;
        updated.psk = updated.channels[index].psk.empty() ?
                      std::string("default") : updated.channels[index].psk;
        request_reconfigure = true;
    }

    if(request_reconfigure && !apply_meshtastic_profile(&updated)) {
        return "ERR unsupported-profile\n";
    }
    if(!phoneapi_persist_meshtastic_opts(updated)) {
        return "ERR persist-failed\n";
    }
    phoneapi_store_runtime_opts(updated, request_reconfigure);
    daemon_event("Daemon SET_CHANNEL_SLOT index=%u role=%s name=%s psk=%s reconfig=%s",
                 index, daemon_channel_role_name(role),
                 role == MESHTASTIC_CHANNEL_ROLE_DISABLED ? "-" :
                 mesh_channel_slot_name(updated, index).c_str(),
                 role == MESHTASTIC_CHANNEL_ROLE_DISABLED ? "-" :
                 (strcmp(psk_text, "default") == 0 ? "default" : "custom"),
                 request_reconfigure ? "yes" : "no");
    return daemon_channels_response(updated);
}

static int base64url_value(char c)
{
    if(c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if(c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if(c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if(c == '-' || c == '+') {
        return 62;
    }
    if(c == '_' || c == '/') {
        return 63;
    }
    return -1;
}

static bool base64url_decode_no_pad(const std::string &text,
                                    std::vector<uint8_t> *out)
{
    uint32_t acc = 0;
    int bits = 0;
    bool saw_pad = false;

    if(!out) {
        return false;
    }
    out->clear();
    for(char c : text) {
        int value;

        if(isspace((unsigned char)c)) {
            continue;
        }
        if(c == '=') {
            saw_pad = true;
            continue;
        }
        if(saw_pad) {
            return false;
        }
        value = base64url_value(c);
        if(value < 0) {
            return false;
        }
        acc = (acc << 6U) | (uint32_t)value;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            out->push_back((uint8_t)((acc >> bits) & 0xffU));
        }
    }
    return !out->empty();
}

static bool meshtastic_channel_url_payload(const std::string &url,
                                           std::string *payload)
{
    std::string clean = trim_ipc_line(url.c_str());
    size_t hash;

    if(!payload) {
        return false;
    }
    payload->clear();
    hash = clean.find('#');
    if(hash != std::string::npos) {
        clean.erase(0, hash + 1U);
    } else {
        const char prefix[] = "meshtastic.org/e/";
        size_t marker = clean.find(prefix);
        if(marker != std::string::npos) {
            clean.erase(0, marker + strlen(prefix));
        }
    }
    while(!clean.empty() && (clean[0] == '/' || clean[0] == '#')) {
        clean.erase(0, 1);
    }
    for(char c : clean) {
        if(isspace((unsigned char)c) || c == '&' || c == '?') {
            break;
        }
        payload->push_back(c);
    }
    return !payload->empty();
}

static bool meshtastic_parse_channel_url(const std::string &url,
                                         phoneapi_channel_update_t *channel,
                                         phoneapi_config_update_t *config)
{
    std::string encoded;
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    bool found = false;

    if(!channel || !config ||
       !meshtastic_channel_url_payload(url, &encoded) ||
       !base64url_decode_no_pad(encoded, &bytes)) {
        return false;
    }
    *channel = phoneapi_channel_update_t();
    *config = phoneapi_config_update_t();
    while(pos < bytes.size()) {
        uint32_t tag;
        uint32_t field;
        uint32_t wire;

        if(!read_varint(bytes.data(), bytes.size(), &pos, &tag)) {
            return false;
        }
        field = tag >> 3U;
        wire = tag & 0x07U;
        if(field == 1U && wire == 2U) {
            std::vector<uint8_t> settings;
            if(!phoneapi_read_length_delimited(bytes, &pos, &settings) ||
               !phoneapi_parse_channel_settings(settings, channel)) {
                return false;
            }
            found = true;
        } else if(field == 2U && wire == 2U) {
            std::vector<uint8_t> lora;
            if(!phoneapi_read_length_delimited(bytes, &pos, &lora) ||
               !phoneapi_parse_lora_config_update(lora, config)) {
                return false;
            }
            found = true;
        } else if(!phoneapi_proto_skip(bytes.data(), bytes.size(), &pos, wire)) {
            return false;
        }
    }
    return found;
}

static bool daemon_prepare_channel_url_import(const std::string &url,
                                              const probe_options_t &opts,
                                              probe_options_t *imported,
                                              bool *request_reconfigure,
                                              std::string *error)
{
    phoneapi_channel_update_t channel;
    phoneapi_config_update_t config;

    if(imported) {
        *imported = opts;
    }
    if(request_reconfigure) {
        *request_reconfigure = false;
    }

    if(!meshtastic_parse_channel_url(url, &channel, &config)) {
        if(error) {
            *error = "invalid-channel-url";
        }
        return false;
    }
    if(!imported || !request_reconfigure) {
        return true;
    }

    if(channel.has_name) {
        imported->channel_name = mesh_clean_text(channel.name);
        *request_reconfigure = true;
    }
    if(channel.has_psk) {
        imported->psk = channel.psk;
        *request_reconfigure = true;
    }
    if(config.has_region && config.region != "UNSET") {
        imported->region = config.region;
        *request_reconfigure = true;
    }
    if(config.has_preset) {
        imported->preset = config.preset;
        *request_reconfigure = true;
    }
    if(config.has_hop_limit) {
        imported->hop_limit = config.hop_limit;
        *request_reconfigure = true;
    }
    if(config.has_tx_power && config.tx_power >= -9 &&
       config.tx_power <= MESHTASTIC_MAX_K230_TX_POWER_DBM) {
        imported->profile.power = (int8_t)config.tx_power;
        imported->manual_power = config.tx_power != 0;
        *request_reconfigure = true;
    }
    if(config.has_channel_num) {
        imported->frequency_slot = config.channel_num;
        *request_reconfigure = true;
    }
    if(*request_reconfigure && !apply_meshtastic_profile(imported)) {
        if(error) {
            *error = "unsupported-channel-config";
        }
        return false;
    }
    return true;
}

static std::string daemon_channel_url_summary(const char *prefix,
                                              const probe_options_t &opts)
{
    char buf[640];
    const char *region = !opts.region.empty() ? opts.region.c_str() :
                         (!opts.resolved_region.empty() ?
                          opts.resolved_region.c_str() :
                          MESHTASTIC_DEFAULT_REGION);
    const char *preset = !opts.preset.empty() ? opts.preset.c_str() :
                         (!opts.resolved_preset.empty() ?
                          opts.resolved_preset.c_str() :
                          MESHTASTIC_DEFAULT_PRESET);
    uint32_t slot = opts.frequency_slot != 0U ? opts.frequency_slot :
                    opts.resolved_slot;

    snprintf(buf, sizeof(buf),
             "%s region=%s preset=%s channel=%s psk=%s hop=%u slot=%u freq=%.6f bw=%.1f sf=%u cr=%u power=%d\n",
             prefix, region, preset,
             opts.channel_name.empty() ? "<preset>" :
             opts.channel_name.c_str(), opts.psk.c_str(),
             opts.hop_limit, slot, opts.profile.freq,
             opts.profile.bandwidth, opts.profile.sf, opts.profile.cr,
             opts.profile.power);
    return std::string(buf);
}

static std::string daemon_preview_channel_url_response(const std::string &url,
                                                       const probe_options_t &opts)
{
    probe_options_t imported;
    bool request_reconfigure = false;
    std::string error;

    if(!daemon_prepare_channel_url_import(url, opts, &imported,
                                          &request_reconfigure, &error)) {
        daemon_event("channel URL preview failed: %s",
                     error.empty() ? "parse" : error.c_str());
        return "ERR " + (error.empty() ? std::string("invalid-channel-url") :
                         error) + "\n";
    }
    daemon_event("channel URL preview region=%s preset=%s channel=%s psk=%s hop=%u slot=%u",
                 imported.region.c_str(), imported.preset.c_str(),
                 imported.channel_name.empty() ? "<preset>" :
                 imported.channel_name.c_str(), imported.psk.c_str(),
                 imported.hop_limit, imported.frequency_slot);
    return daemon_channel_url_summary("OK preview", imported);
}

static std::string daemon_import_channel_url_response(const std::string &url,
                                                      const probe_options_t &opts)
{
    probe_options_t imported;
    bool request_reconfigure = false;
    std::string error;

    if(!daemon_prepare_channel_url_import(url, opts, &imported,
                                          &request_reconfigure, &error)) {
        daemon_event("channel URL import failed: %s",
                     error.empty() ? "parse" : error.c_str());
        return "ERR " + (error.empty() ? std::string("invalid-channel-url") :
                         error) + "\n";
    }
    if(!phoneapi_persist_meshtastic_opts(imported)) {
        daemon_event("channel URL import failed: persist");
        return "ERR persist-failed\n";
    }
    if(request_reconfigure) {
        phoneapi_store_runtime_opts(imported, true);
    }
    daemon_event("channel URL imported region=%s preset=%s channel=%s psk=%s hop=%u slot=%u",
                 imported.region.c_str(), imported.preset.c_str(),
                 imported.channel_name.empty() ? "<preset>" :
                 imported.channel_name.c_str(), imported.psk.c_str(),
                 imported.hop_limit, imported.frequency_slot);
    return daemon_channel_url_summary("OK imported", imported);
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
    char line[1024];
    std::string response = "OK nodes\n";

    if(mesh_node_count == 0U) {
        response += "No nodes seen yet\n";
        return response;
    }
    snprintf(line, sizeof(line), "Node count: %u\n",
             (unsigned)mesh_node_count);
    response += line;
    for(size_t i = 0; i < mesh_node_count; i++) {
        uint32_t age_s = mesh_node_age_seconds(mesh_nodes[i]);
        const char *long_name = mesh_nodes[i].long_name[0] ?
                                mesh_nodes[i].long_name : "-";
        const char *short_name = mesh_nodes[i].short_name[0] ?
                                 mesh_nodes[i].short_name : "-";
        std::string telemetry = telemetry_summary(mesh_nodes[i]);
        const char *route = mesh_nodes[i].has_route_info ?
                            mesh_nodes[i].route_summary : "-";
        const char *neighbor = mesh_nodes[i].has_neighbor_info ?
                               mesh_nodes[i].neighbor_summary : "-";
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
                snprintf(speed_text, sizeof(speed_text), "%um/s",
                         mesh_nodes[i].ground_speed_cms);
            } else {
                snprintf(speed_text, sizeof(speed_text), "-");
            }
            if(mesh_nodes[i].has_ground_track) {
                snprintf(track_text, sizeof(track_text), "%.1fdeg",
                         mesh_nodes[i].ground_track_1e5 / 100.0);
            } else {
                snprintf(track_text, sizeof(track_text), "-");
            }
            snprintf(line, sizeof(line),
                     "0x%08x name=%s short=%s hw=%d key=%s rx=%lu age=%us rssi=%ddBm snr=%.1f pos=%.7f,%.7f alt=%s speed=%s track=%s sats=%u precision=%u time=%u tel=%s trace=%s nbr=%s\n",
                     mesh_nodes[i].node, long_name, short_name,
                     mesh_nodes[i].hw_model,
                     mesh_nodes[i].has_public_key ? "yes" : "no",
                     (unsigned long)mesh_nodes[i].rx_count, age_s,
                     mesh_nodes[i].rssi_dbm, mesh_nodes[i].snr,
                     mesh_nodes[i].latitude_i * 1e-7,
                     mesh_nodes[i].longitude_i * 1e-7,
                     alt_text, speed_text, track_text,
                     mesh_nodes[i].sats_in_view,
                     mesh_nodes[i].precision_bits,
                     mesh_nodes[i].position_timestamp, telemetry.c_str(),
                     route, neighbor);
        } else {
            snprintf(line, sizeof(line),
                     "0x%08x name=%s short=%s hw=%d key=%s rx=%lu age=%us rssi=%ddBm snr=%.1f pos=- tel=%s trace=%s nbr=%s\n",
                     mesh_nodes[i].node, long_name, short_name,
                     mesh_nodes[i].hw_model,
                     mesh_nodes[i].has_public_key ? "yes" : "no",
                     (unsigned long)mesh_nodes[i].rx_count, age_s,
                     mesh_nodes[i].rssi_dbm, mesh_nodes[i].snr,
                     telemetry.c_str(), route, neighbor);
        }
        response += line;
    }
    return response;
}

static std::string daemon_map_response(const probe_options_t &opts,
                                       chip_type_t chip)
{
    char line[1024];
    std::string response;
    uint64_t now = monotonic_us();
    uint64_t last_nmea_ms = 0ULL;
    size_t positioned_count = 0U;
    size_t active_5m_count = 0U;
    size_t active_15m_count = 0U;
    size_t stale_count = 0U;
    size_t visible_waypoints = 0U;
    double self_lat = 0.0;
    double self_lon = 0.0;
    bool self_has_pos = false;
    bool self_fixed = false;
    std::string local_name = mesh_clean_text(opts.node_name);
    std::string local_short;

    if(mesh_gnss.last_nmea_us > 0ULL && now >= mesh_gnss.last_nmea_us) {
        last_nmea_ms = (now - mesh_gnss.last_nmea_us) / 1000ULL;
    }
    for(size_t i = 0; i < mesh_node_count; i++) {
        uint32_t age_s = mesh_node_age_seconds(mesh_nodes[i]);
        if(mesh_nodes[i].has_position) {
            positioned_count++;
        }
        if(age_s <= MESHTASTIC_NODE_ACTIVE_5M_S) {
            active_5m_count++;
        }
        if(age_s <= MESHTASTIC_NODE_ACTIVE_15M_S) {
            active_15m_count++;
        } else {
            stale_count++;
        }
    }
    for(size_t i = 0; i < mesh_waypoint_count; i++) {
        if(mesh_waypoints[i].valid &&
           !mesh_waypoint_is_expired(mesh_waypoints[i], mesh_now_epoch())) {
            visible_waypoints++;
        }
    }
    if(local_name.empty() || local_name == "k230-t-display") {
        local_name = phoneapi_default_node_name(opts);
    }
    local_short = make_short_node_name(local_name);
    if(opts.fixed_position_enabled) {
        self_has_pos = true;
        self_fixed = true;
        self_lat = opts.fixed_position_latitude_i * 1e-7;
        self_lon = opts.fixed_position_longitude_i * 1e-7;
    } else if(mesh_gnss.has_fix &&
              mesh_gnss.position.has_latitude &&
              mesh_gnss.position.has_longitude) {
        self_has_pos = true;
        self_lat = mesh_gnss.position.latitude_i * 1e-7;
        self_lon = mesh_gnss.position.longitude_i * 1e-7;
    }

    snprintf(line, sizeof(line),
             "OK map version=1 chip=%s nodes=%u positioned=%u waypoints=%u "
             "active_5m=%u active_15m=%u stale=%u "
             "nrf9151=%s gps=%s phase=%s lat=%.7f lon=%.7f sats=%u "
             "gnss_sats_seen=%u "
             "nmea_rx=%lu nmea_valid=%lu nmea_nofix=%lu last_nmea_ms=%lu "
             "ttff_ms=%lu ttff_valid=%s\n",
             chip_name(chip), (unsigned)mesh_node_count,
             (unsigned)positioned_count, (unsigned)visible_waypoints,
             (unsigned)active_5m_count, (unsigned)active_15m_count,
             (unsigned)stale_count,
             mesh_gnss.modem_state, mesh_gnss.gps_state,
             nrf9151_gnss_phase(), self_has_pos ? self_lat : 0.0,
             self_has_pos ? self_lon : 0.0,
             mesh_gnss.position.sats_in_view,
             mesh_gnss.position.sats_in_view,
             (unsigned long)mesh_gnss.nmea_rx_count,
             (unsigned long)mesh_gnss.nmea_valid_count,
             (unsigned long)mesh_gnss.nmea_nofix_count,
             (unsigned long)last_nmea_ms,
             (unsigned long)mesh_gnss.ttff_ms,
             mesh_gnss.ttff_valid ? "1" : "0");
    response += line;
    snprintf(line, sizeof(line),
             "SELF id=0x%08x name=%s short=%s has_pos=%s fixed=%s "
             "lat=%.7f lon=%.7f alt=%d sats=%u precision=%u ts=%u\n",
             opts.from_node,
             mesh_ipc_token(local_name.c_str(), 48U).c_str(),
             mesh_ipc_token(local_short.c_str(), 8U).c_str(),
             self_has_pos ? "1" : "0", self_fixed ? "1" : "0",
             self_has_pos ? self_lat : 0.0, self_has_pos ? self_lon : 0.0,
             (mesh_gnss.has_fix && mesh_gnss.position.has_altitude) ?
             mesh_gnss.position.altitude_m :
             (opts.fixed_position_has_altitude ?
              opts.fixed_position_altitude_m : 0),
             mesh_gnss.has_fix ? mesh_gnss.position.sats_in_view : 0U,
             mesh_gnss.has_fix ? mesh_gnss.position.precision_bits : 0U,
             mesh_gnss.has_fix ? mesh_gnss.position.timestamp : 0U);
    response += line;

    for(size_t i = 0; i < mesh_node_count; i++) {
        const mesh_node_entry_t &node = mesh_nodes[i];
        std::string long_name = mesh_ipc_token(
            node.long_name[0] ? node.long_name : "-", 48U);
        std::string short_name = mesh_ipc_token(
            node.short_name[0] ? node.short_name : "-", 12U);
        int battery = node.has_battery_level ? (int)node.battery_level : -1;
        double voltage = node.has_device_voltage ? node.device_voltage : 0.0;
        double ch_util = node.has_channel_utilization ?
                         node.channel_utilization : -1.0;
        double air_tx = node.has_air_util_tx ? node.air_util_tx : -1.0;

        snprintf(line, sizeof(line),
                 "NODE id=0x%08x name=%s short=%s hw=%d key=%s "
                 "age_s=%u rx=%lu rssi=%d snr=%.1f has_pos=%s "
                 "lat=%.7f lon=%.7f alt=%d sats=%u precision=%u ts=%u "
                 "battery=%d voltage=%.2f ch_util=%.1f air_tx=%.2f "
                 "favorite=%s ignored=%s muted=%s\n",
                 node.node, long_name.c_str(), short_name.c_str(),
                 node.hw_model, node.has_public_key ? "1" : "0",
                 mesh_node_age_seconds(node), (unsigned long)node.rx_count,
                 node.rssi_dbm, node.snr, node.has_position ? "1" : "0",
                 node.has_position ? node.latitude_i * 1e-7 : 0.0,
                 node.has_position ? node.longitude_i * 1e-7 : 0.0,
                 node.has_altitude ? node.altitude_m : 0,
                 node.sats_in_view, node.precision_bits,
                 node.position_timestamp, battery, voltage, ch_util, air_tx,
                 node.is_favorite ? "1" : "0",
                 node.is_ignored ? "1" : "0",
                 node.is_muted ? "1" : "0");
        response += line;
    }

    for(size_t i = 0; i < mesh_waypoint_count; i++) {
        const mesh_waypoint_info_t &wp = mesh_waypoints[i];
        uint32_t now_epoch = mesh_now_epoch();

        if(!wp.valid || mesh_waypoint_is_expired(wp, now_epoch)) {
            continue;
        }
        snprintf(line, sizeof(line),
                 "WAYPOINT id=0x%08x from=0x%08x age_s=%u "
                 "lat=%.7f lon=%.7f expire=%u locked=0x%08x "
                 "icon=0x%08x name=%s desc=%s\n",
                 wp.id, wp.from_node, mesh_waypoint_age_seconds(wp),
                 wp.has_latitude ? wp.latitude_i * 1e-7 : 0.0,
                 wp.has_longitude ? wp.longitude_i * 1e-7 : 0.0,
                 wp.expire, wp.locked_to, wp.icon,
                 mesh_ipc_token(wp.name, 40U).c_str(),
                 mesh_ipc_token(wp.description, 96U).c_str());
        response += line;
    }
    return response;
}

static std::string daemon_waypoints_response(void)
{
    char line[512];
    std::string response = "OK waypoints\n";
    uint32_t now_epoch = mesh_now_epoch();
    size_t visible_count = 0U;

    for(size_t i = 0; i < mesh_waypoint_count; i++) {
        if(mesh_waypoints[i].valid &&
           !mesh_waypoint_is_expired(mesh_waypoints[i], now_epoch)) {
            visible_count++;
        }
    }
    if(visible_count == 0U) {
        response += "No waypoints seen yet\n";
        return response;
    }
    snprintf(line, sizeof(line), "Waypoint count: %u\n",
             (unsigned)visible_count);
    response += line;
    for(size_t i = 0; i < mesh_waypoint_count; i++) {
        const mesh_waypoint_info_t &wp = mesh_waypoints[i];
        std::string name;
        std::string desc;

        if(!wp.valid || mesh_waypoint_is_expired(wp, now_epoch)) {
            continue;
        }
        name = mesh_ipc_token(wp.name, 40U);
        desc = mesh_ipc_token(wp.description, 96U);
        snprintf(line, sizeof(line),
                 "wp id=0x%08x from=0x%08x age=%us lat=%.7f lon=%.7f expire=%u locked=0x%08x icon=0x%08x name=%s desc=%s\n",
                 wp.id, wp.from_node, mesh_waypoint_age_seconds(wp),
                 wp.latitude_i * 1e-7, wp.longitude_i * 1e-7, wp.expire,
                 wp.locked_to, wp.icon, name.c_str(), desc.c_str());
        response += line;
    }
    return response;
}

static bool parse_waypoint_degrees(const std::string &text, double min_value,
                                   double max_value, double *value)
{
    char *endp = nullptr;
    double parsed;

    if(!value || text.empty()) {
        return false;
    }
    errno = 0;
    parsed = strtod(text.c_str(), &endp);
    if(errno != 0 || endp == text.c_str()) {
        return false;
    }
    while(endp && *endp && isspace((unsigned char)*endp)) {
        endp++;
    }
    if(endp && *endp) {
        return false;
    }
    if(parsed < min_value || parsed > max_value || parsed == 0.0) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_waypoint_command(const std::string &arg,
                                   mesh_waypoint_info_t *waypoint,
                                   char *errbuf, size_t errlen)
{
    std::string trimmed = trim_ipc_line(arg.c_str());
    size_t first_comma;
    size_t second_comma;
    std::string lat_text;
    std::string lon_text;
    std::string name_text;
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    int64_t lat_i;
    int64_t lon_i;

    if(!waypoint) {
        return false;
    }
    if(trimmed.empty()) {
        snprintf(errbuf, errlen, "empty-waypoint");
        return false;
    }
    first_comma = trimmed.find(',');
    if(first_comma == std::string::npos) {
        snprintf(errbuf, errlen, "expected-lat-lon-name");
        return false;
    }
    second_comma = trimmed.find(',', first_comma + 1U);
    lat_text = trim_ipc_line(trimmed.substr(0, first_comma).c_str());
    if(second_comma == std::string::npos) {
        lon_text = trim_ipc_line(trimmed.substr(first_comma + 1U).c_str());
        name_text = "K230 waypoint";
    } else {
        lon_text = trim_ipc_line(
            trimmed.substr(first_comma + 1U,
                           second_comma - first_comma - 1U).c_str());
        name_text = mesh_clean_text(trimmed.substr(second_comma + 1U));
        if(name_text.empty()) {
            name_text = "K230 waypoint";
        }
    }
    if(!parse_waypoint_degrees(lat_text, -90.0, 90.0, &lat_deg)) {
        snprintf(errbuf, errlen, "invalid-latitude");
        return false;
    }
    if(!parse_waypoint_degrees(lon_text, -180.0, 180.0, &lon_deg)) {
        snprintf(errbuf, errlen, "invalid-longitude");
        return false;
    }
    lat_i = (int64_t)llround(lat_deg * 10000000.0);
    lon_i = (int64_t)llround(lon_deg * 10000000.0);
    if(lat_i < -900000000LL || lat_i > 900000000LL ||
       lon_i < -1800000000LL || lon_i > 1800000000LL) {
        snprintf(errbuf, errlen, "coordinate-out-of-range");
        return false;
    }

    *waypoint = mesh_waypoint_info_t();
    waypoint->valid = true;
    waypoint->has_id = true;
    waypoint->id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
    if(waypoint->id == 0U) {
        waypoint->id = 1U;
    }
    waypoint->has_latitude = true;
    waypoint->has_longitude = true;
    waypoint->latitude_i = (int32_t)lat_i;
    waypoint->longitude_i = (int32_t)lon_i;
    waypoint->expire = mesh_now_epoch() + 86400U;
    waypoint->icon = 0x0001f4cdU;
    snprintf(waypoint->name, sizeof(waypoint->name), "%s",
             name_text.c_str());
    snprintf(waypoint->description, sizeof(waypoint->description),
             "K230 shared waypoint");
    return true;
}

static bool daemon_parse_target_command(const std::string &line,
                                        const char *prefix,
                                        uint32_t *target)
{
    const char *arg;

    if(!prefix || !target) {
        return false;
    }
    if(line.compare(0, strlen(prefix), prefix) != 0) {
        return false;
    }
    arg = line.c_str() + strlen(prefix);
    while(*arg && isspace((unsigned char)*arg)) {
        arg++;
    }
    if(!mesh_parse_u32_text(arg, target) || *target == 0U ||
       meshtastic_node_is_broadcast(*target)) {
        return false;
    }
    return true;
}

static bool mesh_parse_node_id_text(const std::string &text, uint32_t *node)
{
    std::string clean = trim_ipc_line(text.c_str());

    if(!node || clean.empty()) {
        return false;
    }
    if(clean[0] == '!') {
        char *endp = nullptr;
        unsigned long value;

        errno = 0;
        value = strtoul(clean.c_str() + 1, &endp, 16);
        if(errno != 0 || endp == clean.c_str() + 1 || *endp != '\0' ||
           value > 0xffffffffUL) {
            return false;
        }
        *node = (uint32_t)value;
        return true;
    }
    return mesh_parse_u32_text(clean.c_str(), node);
}

static bool mesh_parse_public_key_text(const std::string &text,
                                       uint8_t public_key[
                                           MESHTASTIC_CURVE25519_KEY_LEN])
{
    std::string clean = trim_ipc_line(text.c_str());
    std::vector<uint8_t> bytes;

    if(!public_key || clean.empty()) {
        return false;
    }
    if(mesh_hex_decode_bytes(clean.c_str(), public_key,
                             MESHTASTIC_CURVE25519_KEY_LEN)) {
        return true;
    }
    if(!base64url_decode_no_pad(clean, &bytes) ||
       bytes.size() != MESHTASTIC_CURVE25519_KEY_LEN) {
        return false;
    }
    memcpy(public_key, bytes.data(), MESHTASTIC_CURVE25519_KEY_LEN);
    return true;
}

static std::string daemon_import_node_key_response(const std::string &line,
                                                   const char *prefix)
{
    const char *arg;
    const char *node_start;
    const char *node_end;
    std::string node_text;
    std::string key_text;
    uint32_t node = 0;
    uint8_t public_key[MESHTASTIC_CURVE25519_KEY_LEN];
    mesh_node_entry_t *entry;
    bool replaced = false;

    if(!prefix || line.compare(0, strlen(prefix), prefix) != 0) {
        return "ERR invalid-import-command\n";
    }
    arg = line.c_str() + strlen(prefix);
    while(*arg && isspace((unsigned char)*arg)) {
        arg++;
    }
    node_start = arg;
    while(*arg && !isspace((unsigned char)*arg)) {
        arg++;
    }
    node_end = arg;
    while(*arg && isspace((unsigned char)*arg)) {
        arg++;
    }
    node_text.assign(node_start, (size_t)(node_end - node_start));
    key_text = trim_ipc_line(arg);
    if(!mesh_parse_node_id_text(node_text, &node) || node == 0U ||
       meshtastic_node_is_broadcast(node)) {
        return "ERR invalid-node\n";
    }
    if(!mesh_parse_public_key_text(key_text, public_key)) {
        return "ERR invalid-public-key\n";
    }
    entry = mesh_node_get_or_create(node);
    if(!entry) {
        return "ERR nodedb-full\n";
    }
    if(entry->has_public_key &&
       memcmp(entry->public_key, public_key,
              MESHTASTIC_CURVE25519_KEY_LEN) != 0) {
        replaced = true;
    }
    memcpy(entry->public_key, public_key, MESHTASTIC_CURVE25519_KEY_LEN);
    entry->has_public_key = true;
    entry->last_seen_us = monotonic_us();
    entry->last_seen_epoch = mesh_now_epoch();
    mesh_nodedb_mark_dirty();
    (void)mesh_nodedb_save();
    daemon_event("Node key imported node=0x%08x key=%s replaced=%s",
                 node, mesh_hex_encode_bytes(public_key, 4U).c_str(),
                 replaced ? "yes" : "no");

    char buf[128];
    snprintf(buf, sizeof(buf),
             "OK imported node=0x%08x key=%s replaced=%s\n",
             node, mesh_hex_encode_bytes(public_key, 4U).c_str(),
             replaced ? "yes" : "no");
    return std::string(buf);
}

static std::string daemon_queue_remote_request(
    std::deque<mesh_remote_request_t> *request_queue,
    mesh_remote_request_type_t type,
    uint32_t target)
{
    mesh_remote_request_t request;
    char buf[112];

    if(!request_queue) {
        return "ERR internal\n";
    }
    if(target == 0U || meshtastic_node_is_broadcast(target)) {
        return "ERR invalid-target\n";
    }
    if(request_queue->size() >= MESHTASTIC_DAEMON_REQUEST_QUEUE_MAX) {
        daemon_event("Remote request queue full target=0x%08x type=%s depth=%u",
                     target, mesh_remote_request_name(type),
                     (unsigned)request_queue->size());
        return "ERR queue-full\n";
    }
    request.type = type;
    request.to_node = target;
    request.request_id = mesh_remote_request_alloc_id();
    request_queue->push_back(request);
    snprintf(buf, sizeof(buf), "queue_depth=%u",
             (unsigned)request_queue->size());
    mesh_remote_status_record(request, "queued", buf, 0U);
    daemon_event("Remote request queued target=0x%08x type=%s depth=%u",
                 target, mesh_remote_request_name(type),
                 (unsigned)request_queue->size());
    snprintf(buf, sizeof(buf),
             "OK request queued id=%u target=0x%08x type=%s depth=%u\n",
             request.request_id, target, mesh_remote_request_name(type),
             (unsigned)request_queue->size());
    return std::string(buf);
}

static std::string handle_daemon_command(const std::string &line,
                                         const probe_options_t &opts,
                                         chip_type_t chip,
                                         PhysicalLayer *radio,
                                         std::deque<mesh_send_request_t> *
                                             send_queue,
                                         std::deque<mesh_remote_request_t> *
                                             request_queue)
{
    std::string message;
    uint32_t target = 0U;

    if(line == "STATUS" || line == "status") {
        return daemon_status_response(opts, chip,
                                      send_queue ? send_queue->size() : 0U);
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
    if(line == "MAP" || line == "map") {
        return daemon_map_response(opts, chip);
    }
    if(line == "REQUEST_STATUS" || line == "request_status") {
        return daemon_remote_request_status_response(request_queue);
    }
    if(line == "WAYPOINTS" || line == "waypoints") {
        return daemon_waypoints_response();
    }
    if(line == "CHANNELS" || line == "channels") {
        return daemon_channels_response(opts);
    }
    if(line.compare(0, 17, "SET_CHANNEL_SLOT ") == 0 ||
       line.compare(0, 17, "set_channel_slot ") == 0) {
        return daemon_set_channel_slot_response(line, opts);
    }
    if(line.compare(0, 14, "SEND_WAYPOINT ") == 0 ||
       line.compare(0, 14, "send_waypoint ") == 0) {
        mesh_waypoint_info_t waypoint;
        std::vector<uint8_t> waypoint_proto;
        mesh_send_request_t request;
        char errbuf[96];
        char buf[192];

        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        if(!send_queue) {
            return "ERR internal\n";
        }
        if(mesh_channel_slot_role(opts, mesh_tx_channel_slot_index(opts, 0U)) ==
           MESHTASTIC_CHANNEL_ROLE_DISABLED) {
            return "ERR channel-disabled\n";
        }
        if(!parse_waypoint_command(line.c_str() + 14, &waypoint,
                                   errbuf, sizeof(errbuf))) {
            snprintf(buf, sizeof(buf), "ERR waypoint %s\n", errbuf);
            return std::string(buf);
        }
        if(!encode_waypoint_proto(waypoint, &waypoint_proto)) {
            return "ERR waypoint-encode\n";
        }
        if(send_queue->size() >= MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
            daemon_event("Daemon SEND_WAYPOINT queue full depth=%u",
                         (unsigned)send_queue->size());
            return "ERR queue-full\n";
        }
        request.raw_payload = true;
        request.portnum = MESHTASTIC_WAYPOINT_APP;
        request.payload = waypoint_proto;
        request.channel_index = 0U;
        request.summary = "waypoint";
        send_queue->push_back(request);
        mesh_waypoint_update(opts.from_node, waypoint);
        daemon_event("Daemon SEND_WAYPOINT queued id=0x%08x lat=%.7f lon=%.7f name=%s bytes=%u depth=%u op=%s",
                     waypoint.id, waypoint.latitude_i * 1e-7,
                     waypoint.longitude_i * 1e-7,
                     waypoint.name, (unsigned)waypoint_proto.size(),
                     (unsigned)send_queue->size(), op_name(active_op));
        snprintf(buf, sizeof(buf),
                 "OK waypoint queued id=0x%08x lat=%.7f lon=%.7f bytes=%u depth=%u\n",
                 waypoint.id, waypoint.latitude_i * 1e-7,
                 waypoint.longitude_i * 1e-7,
                 (unsigned)waypoint_proto.size(),
                 (unsigned)send_queue->size());
        return std::string(buf);
    }
    if(line == "CHANNEL_URL" || line == "channel_url" ||
       line == "CHANNELURL" || line == "channelurl") {
        return daemon_channel_url_response(opts);
    }
    if(line == "PUBLISH_NODEINFO" || line == "publish_nodeinfo") {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        mesh_manual_nodeinfo_requested = true;
        daemon_event("Manual NodeInfo publish queued");
        return "OK nodeinfo queued\n";
    }
    if(line == "PUBLISH_POSITION" || line == "publish_position") {
        mesh_position_info_t fixed_position;

        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        if(!opts.position_enabled) {
            return "ERR position-disabled\n";
        }
        if(fixed_position_from_opts(opts, &fixed_position)) {
            char buf[160];

            mesh_manual_position_requested = true;
            daemon_event("Manual Position publish requested fixed lat=%.7f lon=%.7f",
                         fixed_position.latitude_i * 1e-7,
                         fixed_position.longitude_i * 1e-7);
            snprintf(buf, sizeof(buf),
                     "OK position queued fixed lat=%.7f lon=%.7f\n",
                     fixed_position.latitude_i * 1e-7,
                     fixed_position.longitude_i * 1e-7);
            return std::string(buf);
        }
        daemon_event("Manual Position publish requested nrf9151=%s gps=%s",
                     mesh_gnss.modem_state, mesh_gnss.gps_state);
        if(mesh_gnss.probed && !mesh_gnss.present) {
            return "ERR nrf9151-missing\n";
        }
        mesh_manual_position_requested = true;
        if(mesh_gnss.present && !mesh_gnss.has_fix) {
            char buf[128];

            snprintf(buf, sizeof(buf),
                     "OK position waiting gps=%s detail=%s\n",
                     mesh_gnss.gps_state, mesh_gnss.detail);
            return std::string(buf);
        }
        if(mesh_gnss.present && mesh_gnss.has_fix) {
            char buf[160];

            snprintf(buf, sizeof(buf),
                     "OK position queued lat=%.7f lon=%.7f sats=%u\n",
                     mesh_gnss.position.latitude_i * 1e-7,
                     mesh_gnss.position.longitude_i * 1e-7,
                     mesh_gnss.position.sats_in_view);
            return std::string(buf);
        }
        return "OK position probing nrf9151\n";
    }
    if(line == "PUBLISH_TELEMETRY" || line == "publish_telemetry") {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        mesh_manual_device_telemetry_requested = true;
        mesh_manual_environment_telemetry_requested = true;
        daemon_event("Manual Telemetry publish queued");
        return "OK telemetry queued\n";
    }
    if(daemon_parse_target_command(line, "REQUEST_NODEINFO", &target) ||
       daemon_parse_target_command(line, "request_nodeinfo", &target)) {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        return daemon_queue_remote_request(request_queue,
                                           MESH_REMOTE_REQ_NODEINFO,
                                           target);
    }
    if(daemon_parse_target_command(line, "REQUEST_POSITION", &target) ||
       daemon_parse_target_command(line, "request_position", &target)) {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        return daemon_queue_remote_request(request_queue,
                                           MESH_REMOTE_REQ_POSITION,
                                           target);
    }
    if(daemon_parse_target_command(line, "REQUEST_TELEMETRY_DEVICE",
                                   &target) ||
       daemon_parse_target_command(line, "request_telemetry_device",
                                   &target)) {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        return daemon_queue_remote_request(request_queue,
                                           MESH_REMOTE_REQ_TELEMETRY_DEVICE,
                                           target);
    }
    if(daemon_parse_target_command(line, "REQUEST_TELEMETRY_ENV", &target) ||
       daemon_parse_target_command(line, "request_telemetry_env", &target)) {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        return daemon_queue_remote_request(
            request_queue, MESH_REMOTE_REQ_TELEMETRY_ENVIRONMENT, target);
    }
    if(daemon_parse_target_command(line, "REQUEST_TELEMETRY", &target) ||
       daemon_parse_target_command(line, "request_telemetry", &target)) {
        std::string response;

        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        response = daemon_queue_remote_request(
            request_queue, MESH_REMOTE_REQ_TELEMETRY_DEVICE, target);
        if(response.rfind("OK", 0) == 0) {
            std::string second = daemon_queue_remote_request(
                request_queue, MESH_REMOTE_REQ_TELEMETRY_ENVIRONMENT,
                target);
            if(second.rfind("OK", 0) != 0) {
                response += second;
            }
        }
        return response;
    }
    if(daemon_parse_target_command(line, "REQUEST_TRACEROUTE", &target) ||
       daemon_parse_target_command(line, "request_traceroute", &target)) {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        return daemon_queue_remote_request(request_queue,
                                           MESH_REMOTE_REQ_TRACEROUTE,
                                           target);
    }
    if(daemon_parse_target_command(line, "REQUEST_NEIGHBORINFO", &target) ||
       daemon_parse_target_command(line, "request_neighborinfo", &target)) {
        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        return daemon_queue_remote_request(request_queue,
                                           MESH_REMOTE_REQ_NEIGHBORINFO,
                                           target);
    }
    if(line.compare(0, 16, "IMPORT_NODE_KEY ") == 0) {
        return daemon_import_node_key_response(line, "IMPORT_NODE_KEY");
    }
    if(line.compare(0, 16, "import_node_key ") == 0) {
        return daemon_import_node_key_response(line, "import_node_key");
    }
    if(line.compare(0, 19, "IMPORT_CHANNEL_URL ") == 0 ||
       line.compare(0, 19, "import_channel_url ") == 0) {
        message = trim_ipc_line(line.c_str() + 19);
        if(message.empty()) {
            return "ERR empty-channel-url\n";
        }
        return daemon_import_channel_url_response(message, opts);
    }
    if(line.compare(0, 20, "PREVIEW_CHANNEL_URL ") == 0 ||
       line.compare(0, 20, "preview_channel_url ") == 0) {
        message = trim_ipc_line(line.c_str() + 20);
        if(message.empty()) {
            return "ERR empty-channel-url\n";
        }
        return daemon_preview_channel_url_response(message, opts);
    }
    if(line == "PHOTO_DROP_STATUS" || line == "photo_drop_status") {
        return mesh_photo_debug_drop_status_response();
    }
    if(line == "MEDIA_CONFIG" || line == "media_config") {
        return mesh_media_config_status_response();
    }
    if(line.compare(0, 13, "MEDIA_CONFIG ") == 0 ||
       line.compare(0, 13, "media_config ") == 0) {
        return mesh_media_config_set_response(line.c_str() + 13);
    }
    if(line == "PHOTO_DROP_CLEAR" || line == "photo_drop_clear") {
        return mesh_photo_debug_drop_clear_response();
    }
    if(line.compare(0, 15, "PHOTO_DROP_SEQ ") == 0 ||
       line.compare(0, 15, "photo_drop_seq ") == 0) {
        return mesh_photo_debug_drop_set_seq_response(line.c_str() + 15);
    }
    if(line.compare(0, 16, "PHOTO_DROP_RATE ") == 0 ||
       line.compare(0, 16, "photo_drop_rate ") == 0) {
        return mesh_photo_debug_drop_set_rate_response(line.c_str() + 16);
    }
    if(line == "QUIT" || line == "quit") {
        running = 0;
        return "OK quitting\n";
    }
    if(line.compare(0, 16, "SEND_PHOTO_FILE ") == 0 ||
       line.compare(0, 16, "send_photo_file ") == 0) {
        std::vector<uint8_t> jpeg_stream;
        std::vector<uint8_t> invite_payload;
        char photo_errbuf[160];
        char buf[240];
        uint32_t stream_id;
        uint16_t total_packets;
        uint32_t stream_crc;
        uint16_t photo_w = MESHTASTIC_FLRC_PHOTO_MAX_W;
        uint16_t photo_h = MESHTASTIC_FLRC_PHOTO_MAX_H;
        std::string stream_sha256;
        const char *path_arg = line.c_str() + 16;
        std::string path = trim_ipc_line(path_arg);
        std::string chat_path;
        mesh_send_request_t request;

        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        if(!send_queue) {
            return "ERR internal\n";
        }
        if(path.empty()) {
            return "ERR empty-photo-file\n";
        }
        if(mesh_channel_slot_role(opts, mesh_tx_channel_slot_index(opts, 0U)) ==
           MESHTASTIC_CHANNEL_ROLE_DISABLED) {
            return "ERR channel-disabled\n";
        }
        if(chip != CHIP_LR2021 || !active_lr2021) {
            daemon_event("Daemon SEND_PHOTO rejected path=%s chip=%s reason=flrc-needs-lr2021",
                         path.c_str(), chip_name(chip));
            return "ERR photo-requires-lr2021-flrc\n";
        }
        stream_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
        if(stream_id == 0U) {
            stream_id = 1U;
        }
        if(!mesh_photo_prepare_jpeg(path.c_str(), stream_id, &jpeg_stream,
                                    &chat_path, &photo_w, &photo_h,
                                    photo_errbuf, sizeof(photo_errbuf))) {
            daemon_event("Daemon SEND_PHOTO encode failed path=%s reason=%s",
                         path.c_str(), photo_errbuf);
            snprintf(buf, sizeof(buf), "ERR photo-encode %s\n",
                     photo_errbuf);
            return std::string(buf);
        }
        total_packets = (uint16_t)((jpeg_stream.size() +
                                    MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN - 1U) /
                                   MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN);
        if(total_packets == 0U ||
           total_packets > MESHTASTIC_FLRC_PHOTO_MAX_PACKETS) {
            daemon_event("Daemon SEND_PHOTO FLRC packet count invalid stream=0x%08x packets=%u bytes=%u",
                         stream_id, total_packets,
                         (unsigned)jpeg_stream.size());
            return "ERR photo-too-large\n";
        }
        if(send_queue->size() >= MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
            daemon_event("Daemon SEND_PHOTO invite queue full depth=%u",
                         (unsigned)send_queue->size());
            return "ERR queue-full\n";
        }
        stream_crc = crc32_update(0, jpeg_stream.data(), jpeg_stream.size());
        stream_sha256 = mesh_sha256_hex_vector(jpeg_stream);
        invite_payload = mesh_flrc_photo_make_invite_payload(
            stream_id, jpeg_stream, photo_w, photo_h, total_packets);
        request.raw_payload = true;
        request.voice = true;
        request.portnum = MESHTASTIC_PRIVATE_APP;
        request.payload = invite_payload;
        request.channel_index = 0U;
        request.summary = "jpeg-flrc-photo-invite";
        request.flrc_photo_after_tx = true;
        request.flrc_photo_payload = jpeg_stream;
        request.flrc_photo_stream_id = stream_id;
        request.flrc_photo_payload_crc = stream_crc;
        request.flrc_photo_total_packets = total_packets;
        request.flrc_photo_width = photo_w;
        request.flrc_photo_height = photo_h;
        send_queue->push_back(request);
        daemon_chat("TX 0x%08x photo %ux%u jpg packets=%u stream=0x%08x state=queued sha256=%s file=%s",
                    opts.from_node, photo_w, photo_h, total_packets,
                    stream_id, stream_sha256.c_str(),
                    chat_path.empty() ? path.c_str() :
                    chat_path.c_str());
        daemon_event("Daemon SEND_PHOTO queued stream=0x%08x control=mesh-private packets=%u bytes=%u crc=0x%08x sha256=%s depth=%u op=%s",
                     stream_id, total_packets, (unsigned)jpeg_stream.size(),
                     stream_crc, stream_sha256.c_str(),
                     (unsigned)send_queue->size(), op_name(active_op));
        snprintf(buf, sizeof(buf),
                 "OK photo queued stream=0x%08x packets=%u bytes=%u sha256=%s depth=%u\n",
                 stream_id, total_packets, (unsigned)jpeg_stream.size(),
                 stream_sha256.c_str(), (unsigned)send_queue->size());
        return std::string(buf);
    }
    if(line.compare(0, 16, "SEND_VOICE_FILE ") == 0 ||
       line.compare(0, 16, "send_voice_file ") == 0) {
        std::vector<uint8_t> codec2_stream;
        std::vector<uint8_t> invite_payload;
        char codec2_errbuf[128];
        char buf[192];
        uint32_t stream_id;
        uint16_t total_packets;
        uint32_t stream_crc;
        const char *path_arg = line.c_str() + 16;
        std::string path = trim_ipc_line(path_arg);
        std::string chat_path;
        double voice_seconds = 0.0;
        unsigned codec2_duration_ms = 0U;
        mesh_send_request_t request;

        if(!opts.mesh_mode) {
            return "ERR mesh-disabled\n";
        }
        if(!send_queue) {
            return "ERR internal\n";
        }
        if(path.empty()) {
            return "ERR empty-voice-file\n";
        }
        if(mesh_channel_slot_role(opts, mesh_tx_channel_slot_index(opts, 0U)) ==
           MESHTASTIC_CHANNEL_ROLE_DISABLED) {
            return "ERR channel-disabled\n";
        }
        if(chip != CHIP_LR2021 || !active_lr2021) {
            daemon_event("Daemon SEND_VOICE rejected path=%s chip=%s reason=flrc-needs-lr2021",
                         path.c_str(), chip_name(chip));
            return "ERR voice-requires-lr2021-flrc\n";
        }
        stream_id = (uint32_t)(monotonic_us() & 0xffffffffU) ^ ++seq_count;
        if(stream_id == 0U) {
            stream_id = 1U;
        }
        if(!mesh_voice_encode_codec2_pcm_file_stream(path.c_str(),
                                                     &codec2_stream,
                                                     &codec2_duration_ms,
                                                     codec2_errbuf,
                                                     sizeof(codec2_errbuf))) {
            daemon_event("Daemon SEND_VOICE codec2 stream encode failed path=%s reason=%s",
                         path.c_str(), codec2_errbuf);
            snprintf(buf, sizeof(buf), "ERR voice-encode %s\n",
                     codec2_errbuf);
            return std::string(buf);
        }
        total_packets = (uint16_t)((codec2_stream.size() +
                                    MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN - 1U) /
                                   MESHTASTIC_FLRC_VOICE_PAYLOAD_LEN);
        if(total_packets == 0U || total_packets > 64U) {
            daemon_event("Daemon SEND_VOICE FLRC packet count invalid stream=0x%08x packets=%u bytes=%u",
                         stream_id, total_packets,
                         (unsigned)codec2_stream.size());
            return "ERR voice-too-large\n";
        }
        if(send_queue->size() >= MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
            daemon_event("Daemon SEND_VOICE invite queue full depth=%u",
                         (unsigned)send_queue->size());
            return "ERR queue-full\n";
        }
        voice_seconds = (double)codec2_duration_ms / 1000.0;
        stream_crc = crc32_update(0, codec2_stream.data(),
                                  codec2_stream.size());
        if(!mesh_voice_copy_pcm_cache(path, stream_id, &chat_path)) {
            chat_path = path;
        }
        invite_payload = mesh_flrc_voice_make_invite_payload(
            stream_id, codec2_stream, codec2_duration_ms, total_packets,
            MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE);
        request.raw_payload = true;
        request.voice = true;
        request.portnum = MESHTASTIC_PRIVATE_APP;
        request.payload = invite_payload;
        request.channel_index = 0U;
        request.summary = "codec2-flrc-voice-invite";
        request.flrc_voice_after_tx = true;
        request.flrc_voice_payload = codec2_stream;
        request.flrc_voice_stream_id = stream_id;
        request.flrc_voice_duration_ms = codec2_duration_ms;
        request.flrc_voice_payload_crc = stream_crc;
        request.flrc_voice_total_packets = total_packets;
        request.flrc_voice_codec_mode = MESHTASTIC_VOICE_CODEC2_DEFAULT_MODE;
        send_queue->push_back(request);
        daemon_chat("TX 0x%08x voice %.1fs codec=codec2-flrc packets=%u stream=0x%08x state=queued file=%s",
                    opts.from_node, voice_seconds, total_packets, stream_id,
                    chat_path.c_str());
        daemon_event("Daemon SEND_VOICE queued stream=0x%08x codec=codec2-flrc control=mesh-private packets=%u bytes=%u crc=0x%08x depth=%u op=%s",
                     stream_id, total_packets,
                     (unsigned)codec2_stream.size(), stream_crc,
                     (unsigned)send_queue->size(),
                     op_name(active_op));
        snprintf(buf, sizeof(buf),
                 "OK voice queued codec=codec2-flrc stream=0x%08x packets=%u bytes=%u depth=%u\n",
                 stream_id, total_packets, (unsigned)codec2_stream.size(),
                 (unsigned)send_queue->size());
        return std::string(buf);
    }
    if(line.compare(0, 13, "SEND_CHANNEL ") == 0 ||
       line.compare(0, 13, "send_channel ") == 0) {
        const char *arg = line.c_str() + 13;
        char *endp = nullptr;
        uint32_t channel_index;
        mesh_send_request_t request;

        if(!send_queue) {
            return "ERR internal\n";
        }
        errno = 0;
        channel_index = (uint32_t)strtoul(arg, &endp, 10);
        if(errno != 0 || endp == arg ||
           channel_index >= MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
            return "ERR invalid-channel\n";
        }
        while(*endp && isspace((unsigned char)*endp)) {
            endp++;
        }
        message = trim_ipc_line(endp);
        if(message.empty()) {
            return "ERR empty-message\n";
        }
        if(message.size() > MESHTASTIC_MAX_IPC_MESSAGE_LEN) {
            return "ERR message-too-long\n";
        }
        if(mesh_channel_slot_role(opts,
                                  mesh_tx_channel_slot_index(
                                      opts, channel_index)) ==
           MESHTASTIC_CHANNEL_ROLE_DISABLED) {
            return "ERR channel-disabled\n";
        }
        if(send_queue->size() >= MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
            daemon_event("Daemon SEND_CHANNEL queue full slot=%u len=%u depth=%u",
                         channel_index, (unsigned)message.size(),
                         (unsigned)send_queue->size());
            return "ERR queue-full\n";
        }
        request.message = message;
        request.channel_index = channel_index;
        send_queue->push_back(request);
        daemon_event("Daemon SEND_CHANNEL queued slot=%u len=%u depth=%u op=%s",
                     channel_index, (unsigned)message.size(),
                     (unsigned)send_queue->size(), op_name(active_op));
        char buf[112];
        snprintf(buf, sizeof(buf), "OK queued slot=%u len=%u depth=%u\n",
                 channel_index, (unsigned)message.size(),
                 (unsigned)send_queue->size());
        return std::string(buf);
    }
    if(line.compare(0, 8, "SEND_TO ") == 0 ||
       line.compare(0, 8, "send_to ") == 0 ||
       line.compare(0, 12, "SEND_TO_ACK ") == 0 ||
       line.compare(0, 12, "send_to_ack ") == 0) {
        const bool want_ack =
            line.compare(0, 12, "SEND_TO_ACK ") == 0 ||
            line.compare(0, 12, "send_to_ack ") == 0;
        const char *arg = line.c_str() + (want_ack ? 12 : 8);
        char target_text[32];
        size_t target_len = 0;
        uint32_t target = 0;
        mesh_send_request_t request;

        if(!send_queue) {
            return "ERR internal\n";
        }
        while(*arg && isspace((unsigned char)*arg)) {
            arg++;
        }
        while(arg[target_len] && !isspace((unsigned char)arg[target_len]) &&
              target_len + 1U < sizeof(target_text)) {
            target_text[target_len] = arg[target_len];
            target_len++;
        }
        target_text[target_len] = '\0';
        if(target_len == 0U ||
           !mesh_parse_node_id_text(target_text, &target) || target == 0U) {
            return "ERR invalid-target\n";
        }
        arg += target_len;
        while(*arg && isspace((unsigned char)*arg)) {
            arg++;
        }
        message = trim_ipc_line(arg);
        if(message.empty()) {
            return "ERR empty-message\n";
        }
        if(message.size() > MESHTASTIC_MAX_IPC_MESSAGE_LEN) {
            return "ERR message-too-long\n";
        }
        if(send_queue->size() >= MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
            daemon_event("Daemon SEND_TO queue full target=0x%08x len=%u depth=%u",
                         target, (unsigned)message.size(),
                         (unsigned)send_queue->size());
            return "ERR queue-full\n";
        }
        request.message = message;
        request.channel_index = 0U;
        request.has_to_node = true;
        request.to_node = target;
        request.has_want_ack = true;
        request.want_ack = want_ack;
        send_queue->push_back(request);
        daemon_event("Daemon SEND_TO queued target=0x%08x ack=%s len=%u depth=%u op=%s",
                     target, want_ack ? "yes" : "no",
                     (unsigned)message.size(),
                     (unsigned)send_queue->size(), op_name(active_op));
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "OK queued target=0x%08x ack=%s len=%u depth=%u\n",
                 target, want_ack ? "yes" : "no",
                 (unsigned)message.size(),
                 (unsigned)send_queue->size());
        return std::string(buf);
    }
    if(line.compare(0, 5, "SEND ") == 0 || line.compare(0, 5, "send ") == 0) {
        mesh_send_request_t request;

        if(!send_queue) {
            return "ERR internal\n";
        }
        message = trim_ipc_line(line.c_str() + 5);
        if(message.empty()) {
            return "ERR empty-message\n";
        }
        if(message.size() > MESHTASTIC_MAX_IPC_MESSAGE_LEN) {
            return "ERR message-too-long\n";
        }
        if(send_queue->size() >= MESHTASTIC_DAEMON_SEND_QUEUE_MAX) {
            daemon_event("Daemon SEND queue full len=%u depth=%u",
                         (unsigned)message.size(),
                         (unsigned)send_queue->size());
            return "ERR queue-full\n";
        }
        request.message = message;
        request.channel_index = 0U;
        send_queue->push_back(request);
        daemon_event("Daemon SEND queued len=%u depth=%u op=%s",
                     (unsigned)message.size(),
                     (unsigned)send_queue->size(),
                     op_name(active_op));
        char buf[96];
        snprintf(buf, sizeof(buf), "OK queued len=%u depth=%u\n",
                 (unsigned)message.size(), (unsigned)send_queue->size());
        return std::string(buf);
    }
    return "ERR unknown-command\n";
}

static void accept_daemon_clients(int server_fd, const probe_options_t &opts,
                                  chip_type_t chip,
                                  PhysicalLayer *radio,
                                  std::deque<mesh_send_request_t> *send_queue,
                                  std::deque<mesh_remote_request_t> *
                                      request_queue)
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
                response = handle_daemon_command(line, opts, chip, radio,
                                                 send_queue, request_queue);
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
    } else if(opts.client_map) {
        command = "MAP\n";
    } else if(opts.client_request_status) {
        command = "REQUEST_STATUS\n";
    } else if(opts.client_waypoints) {
        command = "WAYPOINTS\n";
    } else if(opts.client_channels) {
        command = "CHANNELS\n";
    } else if(opts.client_set_channel_slot) {
        if(opts.client_set_channel_slot_command.empty()) {
            fprintf(stderr, "--cmd-set-channel-slot value is empty\n");
            return 2;
        }
        command = opts.client_set_channel_slot_command + "\n";
    } else if(opts.client_send_waypoint_requested) {
        if(opts.client_send_waypoint_text.empty()) {
            fprintf(stderr, "--cmd-send-waypoint value is empty\n");
            return 2;
        }
        command = "SEND_WAYPOINT " + opts.client_send_waypoint_text + "\n";
    } else if(opts.client_channel_url) {
        command = "CHANNEL_URL\n";
    } else if(opts.client_publish_nodeinfo) {
        command = "PUBLISH_NODEINFO\n";
    } else if(opts.client_publish_position) {
        command = "PUBLISH_POSITION\n";
    } else if(opts.client_publish_telemetry) {
        command = "PUBLISH_TELEMETRY\n";
    } else if(opts.client_request_nodeinfo) {
        command = "REQUEST_NODEINFO " + opts.client_request_target + "\n";
    } else if(opts.client_request_position) {
        command = "REQUEST_POSITION " + opts.client_request_target + "\n";
    } else if(opts.client_request_telemetry) {
        command = "REQUEST_TELEMETRY " + opts.client_request_target + "\n";
    } else if(opts.client_request_traceroute) {
        command = "REQUEST_TRACEROUTE " + opts.client_request_target + "\n";
    } else if(opts.client_request_neighborinfo) {
        command = "REQUEST_NEIGHBORINFO " + opts.client_request_target + "\n";
    } else if(opts.client_import_node_key) {
        command = "IMPORT_NODE_KEY " + opts.client_import_node + " " +
                  opts.client_import_key + "\n";
    } else if(opts.client_send_to_requested) {
        if(opts.client_send_to_target.empty() ||
           opts.client_send_to_message.empty()) {
            fprintf(stderr, "--cmd-send-to target/message is empty\n");
            return 2;
        }
        command = (opts.client_send_to_ack ? "SEND_TO_ACK " : "SEND_TO ") +
                  opts.client_send_to_target + " " +
                  opts.client_send_to_message + "\n";
    } else if(opts.client_send_voice_requested) {
        if(opts.client_send_voice_path.empty()) {
            fprintf(stderr, "--cmd-send-voice file is empty\n");
            return 2;
        }
        command = "SEND_VOICE_FILE " + opts.client_send_voice_path + "\n";
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
            "  %s --cmd-status|--cmd-log|--cmd-chat|--cmd-nodes|--cmd-map|--cmd-request-status|--cmd-waypoints|--cmd-channels|--cmd-set-channel-slot INDEX ROLE NAME PSK|--cmd-channel-url|--cmd-publish-nodeinfo|--cmd-publish-position|--cmd-publish-telemetry|--cmd-request-nodeinfo NODE|--cmd-request-position NODE|--cmd-request-telemetry NODE|--cmd-request-traceroute NODE|--cmd-request-neighborinfo NODE|--cmd-import-node-key NODE KEY|--cmd-send \"hello\"|--cmd-send-to NODE \"hello\"|--cmd-send-to-ack NODE \"hello\"|--cmd-send-voice FILE|--cmd-send-waypoint \"lat,lon,name\"|--cmd-quit [--socket PATH]\n\n"
            "Daemon options:\n"
            "  --daemon        Run as local Meshtastic socket daemon, implies --mesh\n"
            "  --socket PATH   Default " MESHTASTIC_DEFAULT_SOCKET_PATH "\n"
            "  --cmd-status    Query running daemon status and exit\n"
            "  --cmd-log       Query recent daemon TX/RX event log and exit\n"
            "  --cmd-chat      Query recent decoded text messages and exit\n"
            "  --cmd-nodes     Query recently seen mesh nodes and exit\n"
            "  --cmd-map       Query machine-readable map/node data and exit\n"
            "  --cmd-request-status Query recent remote request state and exit\n"
            "  --cmd-waypoints Query recently received mesh waypoints and exit\n"
            "  --cmd-channels  Query local Meshtastic channel slots and exit\n"
            "  --cmd-set-channel-slot INDEX ROLE NAME PSK  Configure a local channel slot\n"
            "  --cmd-channel-url Query Meshtastic channel sharing URL and exit\n"
            "  --cmd-publish-nodeinfo  Ask daemon to publish this node info now\n"
            "  --cmd-publish-position  Ask daemon to publish current GNSS position now\n"
            "  --cmd-publish-telemetry Ask daemon to publish device and environment telemetry now\n"
            "  --cmd-request-nodeinfo NODE  Ask daemon to request remote node info\n"
            "  --cmd-request-position NODE  Ask daemon to request remote position\n"
            "  --cmd-request-telemetry NODE Ask daemon to request remote telemetry\n"
            "  --cmd-request-traceroute NODE Ask daemon to request remote traceroute\n"
            "  --cmd-request-neighborinfo NODE Ask daemon to request remote neighbor info\n"
            "  --cmd-import-node-key NODE KEY Import 32-byte remote PKI public key, hex or base64\n"
            "  --cmd-send MSG  Ask running daemon to transmit MSG and exit\n"
            "  --cmd-send-to NODE MSG  Ask daemon to transmit MSG to NODE without ACK\n"
            "  --cmd-send-to-ack NODE MSG  Ask daemon to transmit MSG to NODE with ACK\n"
            "  --cmd-send-voice FILE  Ask daemon to encode and transmit 8 kHz S16_LE mono PCM\n"
            "  --cmd-send-waypoint LAT,LON,NAME  Ask daemon to broadcast a waypoint\n"
            "  --cmd-quit      Ask running daemon to exit\n\n"
            "Media options:\n"
            "  --photo-repeat N         FLRC photo data repeat count, 1-3, default 2\n"
            "  --photo-repair-rounds N  FLRC photo repair rounds, 0-4, default 2\n"
            "  --photo-repair-repeat N  FLRC repair data repeat count, 1-3, default 2\n"
            "  --photo-repair-window-ms N  FLRC repair RX window, 3000-20000, default 7000\n"
            "  --photo-cache-ttl-sec N  FLRC photo TX cache TTL, 60-900, default 300\n\n"
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
            "  --no-phoneapi   Disable the nRF52840 PhoneAPI/BLE bridge\n"
            "  --nodeinfo-interval SEC  Periodic daemon NodeInfo interval, default 600\n"
            "  --position       Enable nRF9151 GNSS Position broadcast (default)\n"
            "  --no-position    Disable GNSS Position broadcast\n"
            "  --position-interval SEC  Periodic Position interval, default 900\n"
            "  --fixed-position-i LAT_I,LON_I[,ALT_M]  Use fixed 1e-7 degree position\n"
            "  --no-fixed-position  Clear fixed-position CLI override\n"
            "  --gps-uart PATH  legacy option; GNSS uses launcher manager cache\n"
            "  --telemetry      Enable device telemetry broadcast (default)\n"
            "  --no-telemetry   Disable device telemetry broadcast\n"
            "  --telemetry-interval SEC  Device telemetry interval, default 300\n"
            "  --env-telemetry  Enable AHT20 environment telemetry (default)\n"
            "  --no-env-telemetry Disable AHT20 environment telemetry\n"
            "  --env-telemetry-interval SEC  Environment telemetry interval, default 300\n"
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

static bool phoneapi_pref_bool_text(const std::string &text)
{
    return text == "1" || text == "true" || text == "on" ||
           text == "yes";
}

static void phoneapi_load_meshtastic_channel_slots(probe_options_t *opts)
{
    std::vector<phoneapi_pref_entry_t> entries;
    std::string text;
    uint32_t primary = 0U;
    bool has_channel_slot_pref = false;

    if(!opts || !phoneapi_pref_load(&entries)) {
        return;
    }
    if(phoneapi_pref_get(entries, K230_MESH_PREF_PRIMARY_CHANNEL, &text) &&
       parse_u32(text.c_str(), &primary, 10) &&
       primary < MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        opts->primary_channel_index = primary;
    }
    primary = opts->primary_channel_index;

    for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS; i++) {
        char key[64];
        mesh_channel_slot_t &slot = opts->channels[i];
        uint32_t value;
        bool psk_nondefault = false;

        slot = mesh_channel_slot_t();
        slot.role = (i == primary) ? MESHTASTIC_CHANNEL_ROLE_PRIMARY :
                    MESHTASTIC_CHANNEL_ROLE_DISABLED;

        phoneapi_channel_pref_key(key, sizeof(key), i, "role");
        if(phoneapi_pref_get(entries, key, &text) &&
           parse_u32(text.c_str(), &value, 10) &&
           value <= MESHTASTIC_CHANNEL_ROLE_SECONDARY) {
            slot.role = value;
            has_channel_slot_pref = true;
        }
        phoneapi_channel_pref_key(key, sizeof(key), i, "name");
        if(phoneapi_pref_get(entries, key, &text)) {
            std::string clean = mesh_clean_text(text);

            if(!clean.empty()) {
                slot.name = clean;
            }
            has_channel_slot_pref = true;
        }
        phoneapi_channel_pref_key(key, sizeof(key), i, "psk");
        if(phoneapi_pref_get(entries, key, &text)) {
            std::vector<uint8_t> key_bytes;

            if(parse_psk(text, &key_bytes)) {
                slot.psk = text;
                psk_nondefault = text != "default";
                has_channel_slot_pref = true;
            }
        }
        phoneapi_channel_pref_key(key, sizeof(key), i, "uplink");
        if(phoneapi_pref_get(entries, key, &text)) {
            slot.uplink_enabled = phoneapi_pref_bool_text(text);
            has_channel_slot_pref = true;
        }
        phoneapi_channel_pref_key(key, sizeof(key), i, "downlink");
        if(phoneapi_pref_get(entries, key, &text)) {
            slot.downlink_enabled = phoneapi_pref_bool_text(text);
            has_channel_slot_pref = true;
        }
        phoneapi_channel_pref_key(key, sizeof(key), i, "muted");
        if(phoneapi_pref_get(entries, key, &text)) {
            slot.is_muted = phoneapi_pref_bool_text(text);
            has_channel_slot_pref = true;
        }
        phoneapi_channel_pref_key(key, sizeof(key), i,
                                  "position_precision");
        if(phoneapi_pref_get(entries, key, &text) &&
           parse_u32(text.c_str(), &value, 10)) {
            if(value > 0U) {
                slot.has_position_precision = true;
                slot.position_precision = value;
            }
            has_channel_slot_pref = true;
        }
        slot.configured = (i == primary) ||
                          slot.role != MESHTASTIC_CHANNEL_ROLE_DISABLED ||
                          !slot.name.empty() ||
                          psk_nondefault ||
                          slot.uplink_enabled ||
                          slot.downlink_enabled ||
                          slot.is_muted ||
                          slot.has_position_precision;
    }

    if(!has_channel_slot_pref) {
        return;
    }
    if(primary >= MESHTASTIC_PHONEAPI_MAX_CHANNELS) {
        primary = 0U;
    }
    opts->primary_channel_index = primary;
    opts->channels[primary].configured = true;
    opts->channels[primary].role = MESHTASTIC_CHANNEL_ROLE_PRIMARY;
    for(uint32_t i = 0U; i < MESHTASTIC_PHONEAPI_MAX_CHANNELS; i++) {
        if(i != primary && opts->channels[i].configured &&
           opts->channels[i].role == MESHTASTIC_CHANNEL_ROLE_PRIMARY) {
            opts->channels[i].role = MESHTASTIC_CHANNEL_ROLE_SECONDARY;
        }
    }
    if(!opts->channels[primary].name.empty()) {
        opts->channel_name = opts->channels[primary].name;
    }
    if(!opts->channels[primary].psk.empty()) {
        opts->psk = opts->channels[primary].psk;
    }
}

static bool parse_fixed_position_i(const char *text, probe_options_t *opts)
{
    char buf[96];
    char *lat_text;
    char *lon_text;
    char *alt_text;
    int32_t lat;
    int32_t lon;
    int32_t alt = 0;

    if(!text || !opts || strlen(text) >= sizeof(buf)) {
        return false;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    lat_text = buf;
    lon_text = strchr(lat_text, ',');
    if(!lon_text) {
        return false;
    }
    *lon_text++ = '\0';
    alt_text = strchr(lon_text, ',');
    if(alt_text) {
        *alt_text++ = '\0';
    }
    if(!mesh_parse_i32_text(lat_text, &lat) ||
       !mesh_parse_i32_text(lon_text, &lon)) {
        return false;
    }
    if(alt_text && !mesh_parse_i32_text(alt_text, &alt)) {
        return false;
    }
    if(lat < -900000000 || lat > 900000000 ||
       lon < -1800000000 || lon > 1800000000 ||
       lat == 0 || lon == 0) {
        return false;
    }
    opts->position_enabled = true;
    opts->fixed_position_enabled = true;
    opts->fixed_position_latitude_i = lat;
    opts->fixed_position_longitude_i = lon;
    opts->fixed_position_has_altitude = alt_text && alt_text[0];
    opts->fixed_position_altitude_m = alt;
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
        } else if(strcmp(arg, "--no-phoneapi") == 0) {
            opts->phoneapi_enabled = false;
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
        } else if(strcmp(arg, "--position") == 0) {
            opts->position_enabled = true;
        } else if(strcmp(arg, "--no-position") == 0) {
            opts->position_enabled = false;
        } else if(strcmp(arg, "--position-interval") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10)) {
            if(tmp < 60U) {
                tmp = 60U;
            }
            opts->position_interval_sec = tmp;
        } else if(strcmp(arg, "--fixed-position-i") == 0 &&
                  i + 1 < argc) {
            if(!parse_fixed_position_i(argv[++i], opts)) {
                fprintf(stderr, "Invalid --fixed-position-i value\n");
                return false;
            }
        } else if(strcmp(arg, "--no-fixed-position") == 0) {
            opts->fixed_position_enabled = false;
            opts->fixed_position_has_altitude = false;
        } else if(strcmp(arg, "--gps-uart") == 0 && i + 1 < argc) {
            opts->gps_uart_path = argv[++i];
        } else if(strcmp(arg, "--telemetry") == 0) {
            opts->telemetry_enabled = true;
        } else if(strcmp(arg, "--no-telemetry") == 0) {
            opts->telemetry_enabled = false;
        } else if(strcmp(arg, "--telemetry-interval") == 0 &&
                  i + 1 < argc && parse_u32(argv[++i], &tmp, 10)) {
            if(tmp < 60U) {
                tmp = 60U;
            }
            opts->telemetry_device_interval_sec = tmp;
        } else if(strcmp(arg, "--env-telemetry") == 0) {
            opts->environment_telemetry_enabled = true;
        } else if(strcmp(arg, "--no-env-telemetry") == 0) {
            opts->environment_telemetry_enabled = false;
        } else if(strcmp(arg, "--env-telemetry-interval") == 0 &&
                  i + 1 < argc && parse_u32(argv[++i], &tmp, 10)) {
            if(tmp < 60U) {
                tmp = 60U;
            }
            opts->telemetry_environment_interval_sec = tmp;
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
        } else if(strcmp(arg, "--cmd-map") == 0) {
            opts->client_map = true;
        } else if(strcmp(arg, "--cmd-request-status") == 0) {
            opts->client_request_status = true;
        } else if(strcmp(arg, "--cmd-waypoints") == 0) {
            opts->client_waypoints = true;
        } else if(strcmp(arg, "--cmd-channels") == 0) {
            opts->client_channels = true;
        } else if(strcmp(arg, "--cmd-set-channel-slot") == 0 &&
                  i + 4 < argc) {
            opts->client_set_channel_slot = true;
            opts->client_set_channel_slot_command = "SET_CHANNEL_SLOT ";
            opts->client_set_channel_slot_command += argv[++i];
            opts->client_set_channel_slot_command += " ";
            opts->client_set_channel_slot_command += argv[++i];
            opts->client_set_channel_slot_command += " ";
            opts->client_set_channel_slot_command += argv[++i];
            opts->client_set_channel_slot_command += " ";
            opts->client_set_channel_slot_command += argv[++i];
        } else if(strcmp(arg, "--cmd-channel-url") == 0) {
            opts->client_channel_url = true;
        } else if(strcmp(arg, "--cmd-publish-nodeinfo") == 0) {
            opts->client_publish_nodeinfo = true;
        } else if(strcmp(arg, "--cmd-publish-position") == 0) {
            opts->client_publish_position = true;
        } else if(strcmp(arg, "--cmd-publish-telemetry") == 0) {
            opts->client_publish_telemetry = true;
        } else if(strcmp(arg, "--cmd-request-nodeinfo") == 0 &&
                  i + 1 < argc) {
            opts->client_request_nodeinfo = true;
            opts->client_request_target = argv[++i];
        } else if(strcmp(arg, "--cmd-request-position") == 0 &&
                  i + 1 < argc) {
            opts->client_request_position = true;
            opts->client_request_target = argv[++i];
        } else if(strcmp(arg, "--cmd-request-telemetry") == 0 &&
                  i + 1 < argc) {
            opts->client_request_telemetry = true;
            opts->client_request_target = argv[++i];
        } else if(strcmp(arg, "--cmd-request-traceroute") == 0 &&
                  i + 1 < argc) {
            opts->client_request_traceroute = true;
            opts->client_request_target = argv[++i];
        } else if(strcmp(arg, "--cmd-request-neighborinfo") == 0 &&
                  i + 1 < argc) {
            opts->client_request_neighborinfo = true;
            opts->client_request_target = argv[++i];
        } else if(strcmp(arg, "--cmd-import-node-key") == 0 &&
                  i + 2 < argc) {
            opts->client_import_node_key = true;
            opts->client_import_node = argv[++i];
            opts->client_import_key = argv[++i];
        } else if(strcmp(arg, "--cmd-send-to") == 0 && i + 2 < argc) {
            opts->client_send_to_requested = true;
            opts->client_send_to_ack = false;
            opts->client_send_to_target = argv[++i];
            opts->client_send_to_message = argv[++i];
        } else if(strcmp(arg, "--cmd-send-to-ack") == 0 && i + 2 < argc) {
            opts->client_send_to_requested = true;
            opts->client_send_to_ack = true;
            opts->client_send_to_target = argv[++i];
            opts->client_send_to_message = argv[++i];
        } else if(strcmp(arg, "--cmd-send-voice") == 0 && i + 1 < argc) {
            opts->client_send_voice_requested = true;
            opts->client_send_voice_path = argv[++i];
        } else if(strcmp(arg, "--cmd-send-waypoint") == 0 && i + 1 < argc) {
            opts->client_send_waypoint_requested = true;
            opts->client_send_waypoint_text = argv[++i];
        } else if(strcmp(arg, "--cmd-quit") == 0) {
            opts->client_quit = true;
        } else if(strcmp(arg, "--photo-repeat") == 0 && i + 1 < argc &&
                  parse_u32(argv[++i], &tmp, 10)) {
            mesh_media_cfg.photo_data_repeat = (uint8_t)tmp;
            mesh_media_config_clamp();
        } else if(strcmp(arg, "--photo-repair-rounds") == 0 &&
                  i + 1 < argc && parse_u32(argv[++i], &tmp, 10)) {
            mesh_media_cfg.photo_repair_rounds = (uint8_t)tmp;
            mesh_media_config_clamp();
        } else if(strcmp(arg, "--photo-repair-repeat") == 0 &&
                  i + 1 < argc && parse_u32(argv[++i], &tmp, 10)) {
            mesh_media_cfg.photo_repair_repeat = (uint8_t)tmp;
            mesh_media_config_clamp();
        } else if(strcmp(arg, "--photo-repair-window-ms") == 0 &&
                  i + 1 < argc && parse_u32(argv[++i], &tmp, 10)) {
            mesh_media_cfg.photo_repair_window_ms = tmp;
            mesh_media_config_clamp();
        } else if(strcmp(arg, "--photo-cache-ttl-sec") == 0 &&
                  i + 1 < argc && parse_u32(argv[++i], &tmp, 10)) {
            mesh_media_cfg.photo_tx_cache_ttl_sec = tmp;
            mesh_media_config_clamp();
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

static bool default_mesh_node_name_is_auto(const std::string &name)
{
    if(name.empty() || name == "k230-t-display" ||
       name == "nRF52840" || name == "K230 nRF52840 AT") {
        return true;
    }
    if(name.size() == 9U && name.compare(0, 5, "k230-") == 0) {
        for(size_t i = 5U; i < 9U; i++) {
            if(!isxdigit((unsigned char)name[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static uint32_t default_from_node_from_identity(void)
{
    uint32_t value;

    if(!mesh_pki_public_key_available()) {
        return 0U;
    }
    value = ((uint32_t)mesh_pki_identity.public_key[28] << 24U) |
            ((uint32_t)mesh_pki_identity.public_key[29] << 16U) |
            ((uint32_t)mesh_pki_identity.public_key[30] << 8U) |
            (uint32_t)mesh_pki_identity.public_key[31];
    value |= 0x80000000U;
    if(value == 0U || value == MESHTASTIC_NODENUM_BROADCAST) {
        return 0U;
    }
    return value;
}

static uint32_t default_from_node_from_netdev(const char *ifname)
{
    char path[96];
    char line[96];
    unsigned octets[6];
    FILE *fp;
    uint32_t value;

    if(!ifname) {
        return 0U;
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    fp = fopen(path, "r");
    if(!fp) {
        return 0U;
    }
    if(!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0U;
    }
    fclose(fp);
    if(sscanf(line, "%x:%x:%x:%x:%x:%x",
              &octets[0], &octets[1], &octets[2],
              &octets[3], &octets[4], &octets[5]) != 6) {
        return 0U;
    }
    value = 0x80000000U |
            ((octets[2] & 0xffU) << 24U) |
            ((octets[3] & 0xffU) << 16U) |
            ((octets[4] & 0xffU) << 8U) |
            (octets[5] & 0xffU);
    if(value == 0U || value == MESHTASTIC_NODENUM_BROADCAST) {
        return 0U;
    }
    return value;
}

static void default_from_node(probe_options_t *opts)
{
    uint32_t value;
    bool force_auto;

    if(!opts) {
        return;
    }
    force_auto = default_mesh_node_name_is_auto(opts->node_name);
    if(opts->from_node != 0U && !force_auto) {
        return;
    }
    value = default_from_node_from_identity();
    if(value == 0U) {
        value = default_from_node_from_netdev("wlan0");
    }
    if(value == 0U) {
        value = default_from_node_from_netdev("eth0");
    }
    if(value == 0U) {
        value = djb2_hash(opts->node_name.c_str()) ^ 0x4b230000U;
    }
    if(value == 0U || value == MESHTASTIC_NODENUM_BROADCAST) {
        value = 0x4b230001U;
    }
    if(opts->from_node != 0U && opts->from_node != value) {
        daemon_event("Meshtastic default node id migrated node=%s old=0x%08x new=0x%08x",
                     opts->node_name.c_str(), opts->from_node, value);
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
    active_tx_frame_valid = false;
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
    active_tx_airtime_ms = mesh_radio_airtime_ms(radio, len);
    active_tx_frame = frame;
    active_tx_frame_valid = true;
    active_op_start_us = monotonic_us();
    mesh_history_remember_tx(frame);
    daemon_event("TX start len=%u: %s", (unsigned)len,
                 frame.summary.c_str());
    return 0;
}

static void handle_rx_event(PhysicalLayer *radio, const probe_options_t &opts,
                            chip_type_t chip, SX1262 *sx1262,
                            LR2021 *lr2021,
                            const probe_profile_t *profile)
{
    uint8_t data[MESHTASTIC_MAX_LORA_PAYLOAD_LEN + 1U];
    tx_frame_t followup_frame;
    size_t len;
    int16_t state;
    uint32_t rx_airtime_ms = 0U;
    bool rebroadcast_pending = false;

    if(!radio || active_op != OP_RX) {
        return;
    }

    memset(data, 0, sizeof(data));
    len = radio->getPacketLength();
    if(len >= sizeof(data)) {
        len = sizeof(data) - 1U;
    }
    rx_airtime_ms = mesh_radio_airtime_ms(radio, len);
    state = radio->readData(data, len);
    active_op = OP_IDLE;
    mesh_airtime_log_rx(rx_airtime_ms);
    if(state == RADIOLIB_ERR_NONE) {
        float rssi = radio->getRSSI();
        float snr = radio->getSNR();
        (void)radio->finishReceive();
        rx_count++;
        if(opts.mesh_mode) {
            rebroadcast_pending = process_mesh_rx(opts, radio, chip, sx1262,
                                                  lr2021, profile, data, len,
                                                  rssi, snr, &followup_frame);
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

static void handle_tx_event(PhysicalLayer *radio, const probe_options_t &opts,
                            chip_type_t chip, SX1262 *sx1262,
                            LR2021 *lr2021,
                            const probe_profile_t *profile)
{
    int16_t state;
    tx_frame_t finished_frame;
    bool finished_frame_valid;
    bool flrc_session_ran = false;

    if(!radio || active_op != OP_TX) {
        return;
    }

    (void)opts;
    finished_frame = active_tx_frame;
    finished_frame_valid = active_tx_frame_valid;
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
        mesh_airtime_log_tx(active_tx_airtime_ms);
        daemon_event("TX done: %lu airtime=%ums ch_util=%.1f air_tx=%.2f",
                     (unsigned long)tx_count, active_tx_airtime_ms,
                     mesh_airtime_channel_util_percent(),
                     mesh_airtime_tx_util_percent());
        if(active_tx_frame_valid && !active_tx_frame.want_ack) {
            (void)daemon_chat_update_tx_status(active_tx_frame.packet_id,
                                               "sent");
        }
        if(finished_frame_valid && finished_frame.flrc_voice_after_tx) {
            flrc_session_ran = mesh_flrc_voice_tx_session(
                radio, chip, sx1262, lr2021, profile, finished_frame);
        } else if(finished_frame_valid && finished_frame.flrc_photo_after_tx) {
            flrc_session_ran = mesh_flrc_photo_tx_session(
                radio, chip, sx1262, lr2021, profile, finished_frame);
        }
    } else {
        fprintf(stderr, "TX finish failed: %d %s\n", state, error_name(state));
        if(active_tx_frame_valid) {
            (void)daemon_chat_update_tx_status(active_tx_frame.packet_id,
                                               "tx-failed");
        }
    }
    active_tx_airtime_ms = 0U;
    active_tx_frame_valid = false;
    if(!flrc_session_ran) {
        (void)start_rx(radio);
    } else if(active_op != OP_RX) {
        (void)start_rx(radio);
    }
}

static void handle_radio_event(PhysicalLayer *radio, const probe_options_t &opts,
                               chip_type_t chip, SX1262 *sx1262,
                               LR2021 *lr2021,
                               const probe_profile_t *profile)
{
    if(active_op == OP_TX) {
        handle_tx_event(radio, opts, chip, sx1262, lr2021, profile);
    } else if(active_op == OP_RX) {
        handle_rx_event(radio, opts, chip, sx1262, lr2021, profile);
    }
}

static void handle_delayed_tx(PhysicalLayer *radio, const probe_options_t &opts,
                              uint64_t now_us)
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
    if(!frame.routing_ack) {
        char errbuf[128];
        uint32_t airtime_ms = 0U;

        if(!mesh_frame_airtime_allowed(radio, opts, frame, true,
                                       errbuf, sizeof(errbuf),
                                       &airtime_ms)) {
            mesh_rebroadcast_drop_count++;
            daemon_event("Mesh rebroadcast ChUtil drop id=0x%08x airtime_ms=%u reason=%s drop=%lu",
                         frame.packet_id, airtime_ms, errbuf,
                         (unsigned long)mesh_rebroadcast_drop_count);
            return;
        }
    }
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
        (void)daemon_chat_update_tx_status(
            mesh_ack_retry_queue[best].packet_id, "timeout");
        if(mesh_ack_retry_queue[best].frame.phoneapi_origin) {
            (void)phoneapi_notify_routing_result(
                mesh_ack_retry_queue[best].to_node,
                mesh_ack_retry_queue[best].packet_id,
                MESHTASTIC_ROUTING_ERROR_TIMEOUT,
                "ack-timeout");
        }
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
    (void)daemon_chat_update_tx_status(mesh_ack_retry_queue[best].packet_id,
                                       "retry");
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
    uint64_t last_reliable_hold_log_us = 0;
    bool send_once_started = false;
    bool send_once_finished = false;
    bool send_once_awaiting_ack = false;
    int daemon_fd = -1;
    std::deque<mesh_send_request_t> pending_daemon_sends;
    std::deque<mesh_remote_request_t> pending_remote_requests;

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
                              (opts.client_map ? 1 : 0) +
                              (opts.client_request_status ? 1 : 0) +
                              (opts.client_waypoints ? 1 : 0) +
                              (opts.client_channels ? 1 : 0) +
                              (opts.client_set_channel_slot ? 1 : 0) +
                              (opts.client_channel_url ? 1 : 0) +
                              (opts.client_publish_nodeinfo ? 1 : 0) +
                              (opts.client_publish_position ? 1 : 0) +
                              (opts.client_publish_telemetry ? 1 : 0) +
                              (opts.client_request_nodeinfo ? 1 : 0) +
                              (opts.client_request_position ? 1 : 0) +
                              (opts.client_request_telemetry ? 1 : 0) +
                              (opts.client_request_traceroute ? 1 : 0) +
                              (opts.client_request_neighborinfo ? 1 : 0) +
                              (opts.client_import_node_key ? 1 : 0) +
                              (opts.client_send_to_requested ? 1 : 0) +
                              (opts.client_send_voice_requested ? 1 : 0) +
                              (opts.client_send_waypoint_requested ? 1 : 0) +
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
    if(opts.mesh_mode && !mesh_pki_load_or_create_identity()) {
        fprintf(stderr, "Meshtastic PKI identity unavailable\n");
        return 2;
    }
    phoneapi_load_meshtastic_channel_slots(&opts);
    default_node_name(&opts.node_name);
    default_from_node(&opts);
    if(opts.mesh_mode) {
        if(meshtastic_node_is_broadcast(opts.to_node)) {
            opts.want_ack = false;
        } else if(!opts.want_ack_set) {
            opts.want_ack = true;
        }
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
        printf("Meshtastic channel URL: %s\n",
               meshtastic_channel_url(opts).c_str());
        if(mesh_pki_public_key_available()) {
            printf("Meshtastic PKI: generated=%s public=%s...\n",
                   mesh_pki_identity.generated ? "yes" : "no",
                   mesh_hex_encode_bytes(mesh_pki_identity.public_key,
                                         4U).c_str());
        }
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
        if(opts.mesh_mode) {
            (void)mesh_nodedb_load();
        }
        mesh_next_nodeinfo_us =
            (opts.mesh_mode && opts.advertise_nodeinfo) ? monotonic_us() : 0ULL;
        mesh_next_position_us =
            (opts.mesh_mode && opts.position_enabled) ?
            monotonic_us() + 5000000ULL : 0ULL;
        mesh_next_device_telemetry_us =
            (opts.mesh_mode && opts.telemetry_enabled) ?
            monotonic_us() + 8000000ULL : 0ULL;
        mesh_next_environment_telemetry_us =
            (opts.mesh_mode && opts.environment_telemetry_enabled) ?
            monotonic_us() + 11000000ULL : 0ULL;
        mesh_gnss = nrf9151_gnss_state_t();
        nrf9151_gnss_set_state(opts.position_enabled ? "probing" : "off",
                               opts.position_enabled ? "unavailable" : "off",
                               opts.position_enabled ? "Waiting" :
                               "Position disabled");
        if(opts.mesh_mode && opts.phoneapi_enabled) {
            phoneapi_start(opts);
        } else if(opts.mesh_mode) {
            phoneapi_bridge_set_state(PHONEAPI_BRIDGE_OFFLINE, "disabled");
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
            handle_radio_event(radio, opts, chip, sx1262, lr2021,
                               &opts.profile);
            if(active_op != OP_TX && !opts.auto_tx && send_once_started &&
               !send_once_finished &&
               (!send_once_awaiting_ack || mesh_ack_pending_count() == 0U)) {
                send_once_finished = true;
            }
        }

        if(daemon_fd >= 0) {
            accept_daemon_clients(daemon_fd, opts, chip, radio,
                                  &pending_daemon_sends,
                                  &pending_remote_requests);
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
        handle_delayed_tx(radio, opts, now);
        handle_ack_retry(radio, now);
        mesh_nodedb_maybe_save(now);

        if(opts.mesh_mode && opts.position_enabled) {
            mesh_position_info_t fixed_check;

            if(!fixed_position_from_opts(opts, &fixed_check)) {
                nrf9151_gnss_poll(opts, now);
            }
        }

        if((mesh_manual_nodeinfo_requested ||
            (mesh_next_nodeinfo_us != 0ULL && now >= mesh_next_nodeinfo_us)) &&
           active_op != OP_TX) {
            tx_frame_t frame;
            bool manual_publish = mesh_manual_nodeinfo_requested;
            uint64_t interval_us =
                (uint64_t)opts.nodeinfo_interval_sec * 1000000ULL;

            mesh_manual_nodeinfo_requested = false;
            if(build_mesh_nodeinfo_frame(opts, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, true,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    mesh_nodeinfo_drop_count++;
                    mesh_next_nodeinfo_us = opts.advertise_nodeinfo ?
                                           now + MESHTASTIC_NODEINFO_RETRY_US :
                                           0ULL;
                    daemon_event("NodeInfo ChUtil deferred node=%s manual=%s airtime_ms=%u reason=%s",
                                 opts.node_name.c_str(),
                                 manual_publish ? "yes" : "no",
                                 airtime_ms, errbuf);
                } else if(start_tx(radio, frame) == 0) {
                    mesh_nodeinfo_tx_count++;
                    mesh_next_nodeinfo_us = opts.advertise_nodeinfo ?
                                           now + interval_us : 0ULL;
                    daemon_event("NodeInfo TX start node=%s from=0x%08x manual=%s",
                                 opts.node_name.c_str(), opts.from_node,
                                 manual_publish ? "yes" : "no");
                } else {
                    mesh_nodeinfo_drop_count++;
                    mesh_next_nodeinfo_us = opts.advertise_nodeinfo ?
                                           now + MESHTASTIC_NODEINFO_RETRY_US :
                                           0ULL;
                    daemon_event("NodeInfo TX start failed node=%s manual=%s",
                                 opts.node_name.c_str(),
                                 manual_publish ? "yes" : "no");
                }
            } else {
                mesh_nodeinfo_drop_count++;
                mesh_next_nodeinfo_us = opts.advertise_nodeinfo ?
                                       now + MESHTASTIC_NODEINFO_RETRY_US :
                                       0ULL;
                daemon_event("NodeInfo build failed node=%s manual=%s",
                             opts.node_name.c_str(),
                             manual_publish ? "yes" : "no");
            }
        }

        if((mesh_manual_position_requested ||
            (mesh_next_position_us != 0ULL && now >= mesh_next_position_us)) &&
           active_op != OP_TX) {
            tx_frame_t frame;
            mesh_position_info_t position;
            bool manual_publish = mesh_manual_position_requested;
            bool using_fixed = fixed_position_from_opts(opts, &position);
            bool position_ready = using_fixed;
            uint64_t interval_us =
                (uint64_t)opts.position_interval_sec * 1000000ULL;

            mesh_manual_position_requested = false;
            if(!using_fixed) {
                (void)nrf9151_gnss_apply_cache_fix(false);
            }
            if(!using_fixed && (!mesh_gnss.present || !mesh_gnss.has_fix)) {
                mesh_position_drop_count++;
                mesh_next_position_us = opts.position_enabled ?
                                       now + MESHTASTIC_POSITION_RETRY_US :
                                       0ULL;
                daemon_event("Position TX skipped nrf9151=%s gps=%s manual=%s",
                             mesh_gnss.modem_state, mesh_gnss.gps_state,
                             manual_publish ? "yes" : "no");
            } else if(!using_fixed) {
                position = mesh_gnss.position;
                position_ready = true;
            }

            if(position_ready &&
               build_mesh_position_frame(opts, position, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, true,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    mesh_position_drop_count++;
                    mesh_next_position_us = opts.position_enabled ?
                                           now + MESHTASTIC_POSITION_RETRY_US :
                                           0ULL;
                    daemon_event("Position ChUtil deferred from=0x%08x manual=%s airtime_ms=%u reason=%s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no",
                                 airtime_ms, errbuf);
                } else if(start_tx(radio, frame) == 0) {
                    mesh_position_tx_count++;
                    mesh_next_position_us = opts.position_enabled ?
                                           now + interval_us : 0ULL;
                    mesh_node_update_position(opts.from_node, position);
                    daemon_event("Position TX start from=0x%08x source=%s lat=%.7f lon=%.7f sats=%u manual=%s",
                                 opts.from_node,
                                 using_fixed ? "fixed" : "gnss",
                                 position.latitude_i * 1e-7,
                                 position.longitude_i * 1e-7,
                                 position.sats_in_view,
                                 manual_publish ? "yes" : "no");
                } else {
                    mesh_position_drop_count++;
                    mesh_next_position_us = opts.position_enabled ?
                                           now + MESHTASTIC_POSITION_RETRY_US :
                                           0ULL;
                    daemon_event("Position TX start failed from=0x%08x manual=%s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no");
                }
            } else if(position_ready) {
                mesh_position_drop_count++;
                mesh_next_position_us = opts.position_enabled ?
                                       now + MESHTASTIC_POSITION_RETRY_US :
                                       0ULL;
                daemon_event("Position build failed from=0x%08x manual=%s",
                             opts.from_node,
                             manual_publish ? "yes" : "no");
            }
        }

        if((mesh_manual_device_telemetry_requested ||
            (mesh_next_device_telemetry_us != 0ULL &&
             now >= mesh_next_device_telemetry_us)) &&
           active_op != OP_TX) {
            tx_frame_t frame;
            mesh_telemetry_info_t telemetry;
            bool manual_publish = mesh_manual_device_telemetry_requested;
            uint64_t interval_us =
                (uint64_t)opts.telemetry_device_interval_sec * 1000000ULL;

            mesh_manual_device_telemetry_requested = false;
            if(mesh_collect_device_telemetry(&telemetry) &&
               build_mesh_telemetry_frame(opts, telemetry, false, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, true,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    mesh_telemetry_drop_count++;
                    mesh_next_device_telemetry_us = opts.telemetry_enabled ?
                        now + MESHTASTIC_TELEMETRY_RETRY_US : 0ULL;
                    daemon_event("Telemetry ChUtil deferred type=device from=0x%08x manual=%s airtime_ms=%u reason=%s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no",
                                 airtime_ms, errbuf);
                } else if(start_tx(radio, frame) == 0) {
                    mesh_telemetry_tx_count++;
                    mesh_next_device_telemetry_us = opts.telemetry_enabled ?
                                                   now + interval_us : 0ULL;
                    mesh_node_update_telemetry(opts.from_node, telemetry);
                    daemon_event("Telemetry TX start type=device from=0x%08x manual=%s %s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no",
                                 telemetry_summary(telemetry).c_str());
                } else {
                    mesh_telemetry_drop_count++;
                    mesh_next_device_telemetry_us = opts.telemetry_enabled ?
                        now + MESHTASTIC_TELEMETRY_RETRY_US : 0ULL;
                    daemon_event("Telemetry TX start failed type=device from=0x%08x manual=%s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no");
                }
            } else {
                mesh_telemetry_drop_count++;
                mesh_next_device_telemetry_us = opts.telemetry_enabled ?
                    now + MESHTASTIC_TELEMETRY_RETRY_US : 0ULL;
                daemon_event("Telemetry build failed type=device from=0x%08x manual=%s",
                             opts.from_node,
                             manual_publish ? "yes" : "no");
            }
        }

        if((mesh_manual_environment_telemetry_requested ||
            (mesh_next_environment_telemetry_us != 0ULL &&
             now >= mesh_next_environment_telemetry_us)) &&
           active_op != OP_TX) {
            tx_frame_t frame;
            mesh_telemetry_info_t telemetry;
            bool manual_publish = mesh_manual_environment_telemetry_requested;
            uint64_t interval_us =
                (uint64_t)opts.telemetry_environment_interval_sec *
                1000000ULL;

            mesh_manual_environment_telemetry_requested = false;
            if(mesh_collect_environment_telemetry(&telemetry) &&
               build_mesh_telemetry_frame(opts, telemetry, true, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, true,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    mesh_telemetry_drop_count++;
                    mesh_next_environment_telemetry_us =
                        opts.environment_telemetry_enabled ?
                        now + MESHTASTIC_TELEMETRY_RETRY_US : 0ULL;
                    daemon_event("Telemetry ChUtil deferred type=environment from=0x%08x manual=%s airtime_ms=%u reason=%s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no",
                                 airtime_ms, errbuf);
                } else if(start_tx(radio, frame) == 0) {
                    mesh_telemetry_tx_count++;
                    mesh_next_environment_telemetry_us =
                        opts.environment_telemetry_enabled ? now + interval_us :
                        0ULL;
                    mesh_node_update_telemetry(opts.from_node, telemetry);
                    daemon_event("Telemetry TX start type=environment from=0x%08x manual=%s %s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no",
                                 telemetry_summary(telemetry).c_str());
                } else {
                    mesh_telemetry_drop_count++;
                    mesh_next_environment_telemetry_us =
                        opts.environment_telemetry_enabled ?
                        now + MESHTASTIC_TELEMETRY_RETRY_US : 0ULL;
                    daemon_event("Telemetry TX start failed type=environment from=0x%08x manual=%s",
                                 opts.from_node,
                                 manual_publish ? "yes" : "no");
                }
            } else {
                mesh_telemetry_drop_count++;
                mesh_next_environment_telemetry_us =
                    opts.environment_telemetry_enabled ?
                    now + MESHTASTIC_TELEMETRY_RETRY_US : 0ULL;
                daemon_event("Telemetry build skipped type=environment from=0x%08x manual=%s",
                             opts.from_node,
                             manual_publish ? "yes" : "no");
            }
        }

        if(!pending_remote_requests.empty() && active_op != OP_TX) {
            tx_frame_t frame;
            mesh_remote_request_t request = pending_remote_requests.front();
            if(build_mesh_remote_request_frame(opts, request, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, false,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    if(now - last_reliable_hold_log_us > 2000000ULL) {
                        char detail[128];

                        snprintf(detail, sizeof(detail),
                                 "airtime_ms=%u_reason=%s", airtime_ms,
                                 errbuf);
                        mesh_remote_status_record(request, "held", detail,
                                                  airtime_ms);
                        daemon_event("Remote request ChUtil held target=0x%08x type=%s airtime_ms=%u reason=%s depth=%u",
                                     request.to_node,
                                     mesh_remote_request_name(request.type),
                                     airtime_ms, errbuf,
                                     (unsigned)pending_remote_requests.size());
                        last_reliable_hold_log_us = now;
                    }
                } else {
                    pending_remote_requests.pop_front();
                    {
                        char detail[80];

                        snprintf(detail, sizeof(detail), "queue_depth=%u",
                                 (unsigned)pending_remote_requests.size());
                        mesh_remote_status_record(request, "sending", detail,
                                                  airtime_ms,
                                                  frame.packet_id);
                    }
                    daemon_event("Remote request dequeue target=0x%08x type=%s depth=%u airtime_ms=%u",
                                 request.to_node,
                                 mesh_remote_request_name(request.type),
                                 (unsigned)pending_remote_requests.size(),
                                 airtime_ms);
                    if(start_tx(radio, frame) == 0) {
                        mesh_remote_status_record(request, "sent",
                                                  "radio_tx_started",
                                                  airtime_ms,
                                                  frame.packet_id);
                        if(frame.want_ack) {
                            (void)mesh_ack_track_frame(frame);
                        }
                    } else {
                        mesh_remote_status_record(request, "tx-failed",
                                                  "start_tx_failed",
                                                  airtime_ms);
                        daemon_event("Remote request TX start failed target=0x%08x type=%s",
                                     request.to_node,
                                     mesh_remote_request_name(request.type));
                    }
                }
            } else {
                pending_remote_requests.pop_front();
                mesh_remote_status_record(request, "build-failed",
                                          "frame_build_failed", 0U);
                daemon_event("Remote request build failed target=0x%08x type=%s",
                             request.to_node,
                             mesh_remote_request_name(request.type));
            }
        }

        if(!pending_daemon_sends.empty() && active_op != OP_TX) {
            tx_frame_t frame;
            mesh_send_request_t request = pending_daemon_sends.front();
            probe_options_t tx_opts = opts;
            if(request.has_to_node) {
                tx_opts.to_node = request.to_node;
            }
            if(request.has_want_ack) {
                tx_opts.want_ack = request.want_ack;
            }
            if(tx_opts.want_ack && mesh_ack_pending_count() > 0U) {
                if(now - last_reliable_hold_log_us > 2000000ULL) {
                    daemon_event("Reliable daemon SEND held pending_ack=%u depth=%u",
                                 mesh_ack_pending_count(),
                                 (unsigned)pending_daemon_sends.size());
                    last_reliable_hold_log_us = now;
                }
            } else {
                bool built = request.raw_payload ?
                    build_tx_data_frame(tx_opts, request.portnum,
                                        request.payload,
                                        request.channel_index,
                                        request.summary.c_str(), &frame) :
                    build_tx_frame(tx_opts, request.message,
                                   request.channel_index, &frame);
                if(built) {
                    char errbuf[128];
                    uint32_t airtime_ms = 0U;
                    bool polite = request.voice ||
                                  (request.raw_payload &&
                                   (request.portnum == MESHTASTIC_WAYPOINT_APP ||
                                    request.portnum == MESHTASTIC_POSITION_APP ||
                                    request.portnum == MESHTASTIC_NODEINFO_APP ||
                                    request.portnum == MESHTASTIC_TELEMETRY_APP));

                    if(request.flrc_voice_after_tx) {
                        frame.flrc_voice_after_tx = true;
                        frame.flrc_voice_payload = request.flrc_voice_payload;
                        frame.flrc_voice_stream_id =
                            request.flrc_voice_stream_id;
                        frame.flrc_voice_duration_ms =
                            request.flrc_voice_duration_ms;
                        frame.flrc_voice_payload_crc =
                            request.flrc_voice_payload_crc;
                        frame.flrc_voice_total_packets =
                            request.flrc_voice_total_packets;
                        frame.flrc_voice_codec_mode =
                            request.flrc_voice_codec_mode;
                    }
                    if(request.flrc_photo_after_tx) {
                        frame.flrc_photo_after_tx = true;
                        frame.flrc_photo_payload = request.flrc_photo_payload;
                        frame.flrc_photo_stream_id =
                            request.flrc_photo_stream_id;
                        frame.flrc_photo_payload_crc =
                            request.flrc_photo_payload_crc;
                        frame.flrc_photo_total_packets =
                            request.flrc_photo_total_packets;
                        frame.flrc_photo_width = request.flrc_photo_width;
                        frame.flrc_photo_height = request.flrc_photo_height;
                    }

                    if(!mesh_frame_airtime_allowed(radio, tx_opts, frame,
                                                   polite,
                                                   errbuf, sizeof(errbuf),
                                                   &airtime_ms)) {
                        if(now - last_reliable_hold_log_us > 2000000ULL) {
                            daemon_event("Daemon SEND ChUtil held slot=%u target=0x%08x port=%u len=%u airtime_ms=%u reason=%s depth=%u",
                                         request.channel_index,
                                         tx_opts.to_node,
                                         request.raw_payload ? request.portnum :
                                         MESHTASTIC_TEXT_MESSAGE_APP,
                                         request.raw_payload ?
                                         (unsigned)request.payload.size() :
                                         (unsigned)request.message.size(),
                                         airtime_ms, errbuf,
                                         (unsigned)pending_daemon_sends.size());
                            last_reliable_hold_log_us = now;
                        }
                    } else {
                        pending_daemon_sends.pop_front();
                        daemon_event("Daemon SEND dequeue depth=%u slot=%u target=0x%08x ack=%s port=%u len=%u airtime_ms=%u",
                                     (unsigned)pending_daemon_sends.size(),
                                     request.channel_index,
                                     tx_opts.to_node,
                                     tx_opts.want_ack ? "yes" : "no",
                                     request.raw_payload ? request.portnum :
                                     MESHTASTIC_TEXT_MESSAGE_APP,
                                     request.raw_payload ?
                                     (unsigned)request.payload.size() :
                                     (unsigned)request.message.size(),
                                     airtime_ms);
                        if(start_tx(radio, frame) == 0 && tx_opts.mesh_mode) {
                            bool ack_tracked = true;

                            if(frame.want_ack) {
                                ack_tracked = mesh_ack_track_frame(frame);
                            }
                            if(!request.voice) {
                                std::string clean =
                                    mesh_clean_text(request.message);
                                if(!clean.empty()) {
                                    daemon_chat("TX 0x%08x id=0x%08x mode=%s to=0x%08x ch=%u ack=%s: %s",
                                                opts.from_node, frame.packet_id,
                                                request.has_to_node ?
                                                "direct" : "channel",
                                                frame.to_node,
                                                request.channel_index,
                                                frame.want_ack ?
                                                (ack_tracked ? "pending" :
                                                 "dropped") :
                                                "air",
                                                clean.c_str());
                                }
                            }
                        } else {
                            daemon_event("Daemon SEND start failed slot=%u port=%u len=%u",
                                         request.channel_index,
                                         request.raw_payload ? request.portnum :
                                         MESHTASTIC_TEXT_MESSAGE_APP,
                                         request.raw_payload ?
                                         (unsigned)request.payload.size() :
                                         (unsigned)request.message.size());
                        }
                    }
                } else {
                    pending_daemon_sends.pop_front();
                    daemon_event("Daemon SEND build failed slot=%u port=%u len=%u",
                                 request.channel_index,
                                 request.raw_payload ? request.portnum :
                                 MESHTASTIC_TEXT_MESSAGE_APP,
                                 request.raw_payload ?
                                 (unsigned)request.payload.size() :
                                 (unsigned)request.message.size());
                }
            }
        }

        if(active_op != OP_TX) {
            phoneapi_mesh_tx_t phoneapi_tx;
            if(phoneapi_pending_mesh_tx_wants_ack() &&
               mesh_ack_pending_count() > 0U) {
                if(now - last_reliable_hold_log_us > 2000000ULL) {
                    daemon_event("Reliable PhoneAPI TX held pending_ack=%u",
                                 mesh_ack_pending_count());
                    last_reliable_hold_log_us = now;
                }
            } else if(phoneapi_take_mesh_tx(&phoneapi_tx)) {
                tx_frame_t frame;
                if(build_phoneapi_mesh_data_frame(opts, phoneapi_tx, &frame)) {
                    char errbuf[128];
                    uint32_t airtime_ms = 0U;

                    if(!mesh_frame_airtime_allowed(radio, opts, frame, false,
                                                   errbuf, sizeof(errbuf),
                                                   &airtime_ms)) {
                        daemon_event("PhoneAPI TX ChUtil rejected port=%u payload=%u airtime_ms=%u reason=%s",
                                     phoneapi_tx.data.portnum,
                                     (unsigned)phoneapi_tx.data.payload.size(),
                                     airtime_ms, errbuf);
                        if(phoneapi_tx.packet_id != 0U) {
                            (void)phoneapi_notify_routing_result(
                                frame.to_node, phoneapi_tx.packet_id,
                                MESHTASTIC_ROUTING_ERROR_DUTY_CYCLE_LIMIT,
                                errbuf);
                        }
                    } else if(start_tx(radio, frame) == 0 && opts.mesh_mode) {
                        bool ack_tracked = true;
                        if(frame.want_ack) {
                            ack_tracked = mesh_ack_track_frame(frame);
                        }
                        if(phoneapi_tx.data.portnum ==
                           MESHTASTIC_TEXT_MESSAGE_APP &&
                           !phoneapi_tx.data.payload.empty()) {
                            std::string text(
                                (const char *)phoneapi_tx.data.payload.data(),
                                phoneapi_tx.data.payload.size());
                            std::string clean = mesh_clean_text(text);
                            if(!clean.empty()) {
                                daemon_chat("TX 0x%08x id=0x%08x mode=%s to=0x%08x ch=%u ack=%s: %s",
                                            opts.from_node, frame.packet_id,
                                            meshtastic_node_is_broadcast(
                                                frame.to_node) ?
                                            "channel" : "direct",
                                            frame.to_node,
                                            phoneapi_tx.channel_index,
                                            frame.want_ack ?
                                            (ack_tracked ? "pending" :
                                             "dropped") :
                                            "air",
                                            clean.c_str());
                            }
                        }
                    } else if(phoneapi_tx.packet_id != 0U) {
                        (void)phoneapi_notify_routing_result(
                            frame.to_node, phoneapi_tx.packet_id,
                            MESHTASTIC_ROUTING_ERROR_NO_INTERFACE,
                            "tx-start-failed");
                    }
                } else {
                    daemon_event("PhoneAPI TX build failed port=%u payload=%u",
                                 phoneapi_tx.data.portnum,
                                 (unsigned)phoneapi_tx.data.payload.size());
                    if(phoneapi_tx.packet_id != 0U) {
                        (void)phoneapi_notify_routing_result(
                            phoneapi_tx.to_node, phoneapi_tx.packet_id,
                            MESHTASTIC_ROUTING_ERROR_TOO_LARGE,
                            "tx-build-failed");
                    }
                }
            }
        }

        if(!opts.send_once.empty() && !send_once_started &&
           active_op != OP_TX) {
            tx_frame_t frame;
            if(build_tx_frame(opts, opts.send_once, 0U, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, false,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    daemon_event("Send-once ChUtil rejected airtime_ms=%u reason=%s",
                                 airtime_ms, errbuf);
                    send_once_finished = true;
                } else if(start_tx(radio, frame) == 0) {
                    send_once_started = true;
                    send_once_awaiting_ack = frame.want_ack;
                    if(frame.want_ack) {
                        (void)mesh_ack_track_frame(frame);
                    }
                } else {
                    send_once_finished = true;
                }
            } else {
                send_once_finished = true;
            }
        }

        if(opts.auto_tx && active_op != OP_TX &&
           now - last_tx_us >= (uint64_t)opts.interval_ms * 1000ULL) {
            tx_frame_t frame;
            last_tx_us = now;
            if(build_tx_frame(opts, opts.message, 0U, &frame)) {
                char errbuf[128];
                uint32_t airtime_ms = 0U;

                if(!mesh_frame_airtime_allowed(radio, opts, frame, true,
                                               errbuf, sizeof(errbuf),
                                               &airtime_ms)) {
                    daemon_event("Auto TX ChUtil skipped airtime_ms=%u reason=%s",
                                 airtime_ms, errbuf);
                } else if(start_tx(radio, frame) == 0 && frame.want_ack) {
                    (void)mesh_ack_track_frame(frame);
                }
            }
        }

        if(active_op == OP_TX &&
           elapsed_after(now, active_op_start_us,
                         tx_poll_finish_delay_us(active_tx_len))) {
            handle_tx_event(radio, opts, chip, sx1262, lr2021,
                            &opts.profile);
            if(send_once_started &&
               (!send_once_awaiting_ack || mesh_ack_pending_count() == 0U)) {
                send_once_finished = true;
            }
        }

        if(active_op == OP_TX &&
           elapsed_after(now, active_op_start_us, 15000000ULL)) {
            fprintf(stderr, "TX timeout watchdog\n");
            active_op = OP_IDLE;
            active_tx_airtime_ms = 0U;
            active_tx_frame_valid = false;
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
    if(opts.daemon_mode && opts.mesh_mode && mesh_nodedb_dirty) {
        (void)mesh_nodedb_save();
    }
    nrf9151_gnss_close();
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
