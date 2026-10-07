#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "modules/LR2021/LR2021.h"

#define BENCH_SPI_DEV "/dev/spidev0.0"
#define BENCH_SPI_SPEED_DEFAULT 16000000U
#define BENCH_PIN_CS 14U
#define BENCH_PIN_RST 5U
#define BENCH_PIN_BUSY 19U
#define BENCH_PIN_DIO1 20U
#define BENCH_PIN_POWER 44U
#define BENCH_LR2021_IRQ_DIO_NUM 11U
#define BENCH_PAYLOAD_LEN_DEFAULT 252U
#define BENCH_POWER_DEFAULT 4
#define BENCH_FREQ_DEFAULT 2400.0f
#define BENCH_BR_DEFAULT 2600U
#define BENCH_DURATION_DEFAULT 15U
#define BENCH_RX_POLL_US_DEFAULT 50U
#define FLRC_VIDEO_MAGIC 0x564C464BU
#define FLRC_VIDEO_VERSION 1U
#define FLRC_VIDEO_TYPE_START 1U
#define FLRC_VIDEO_TYPE_DATA 2U
#define FLRC_VIDEO_TYPE_END 3U
#define FLRC_VIDEO_TYPE_NACK 4U
#define FLRC_VIDEO_TYPE_DONE 5U
#define FLRC_VIDEO_HDR_LEN 24U
#define FLRC_VIDEO_DEFAULT_FILE "/root/videos/video02.mp4"
#define FLRC_VIDEO_DEFAULT_OUT_DIR "/root/videos/flrc_rx"
#define FLRC_VIDEO_START_REPEAT 18U
#define FLRC_VIDEO_END_REPEAT 10U
#define FLRC_VIDEO_CTRL_REPEAT 24U
#define FLRC_VIDEO_REPEAT_GAP_US 3500U
#define FLRC_VIDEO_CTRL_REPLY_DELAY_MS 220U
#define FLRC_VIDEO_DONE_REPEAT_MIN_MS 5000U
#define FLRC_VIDEO_TX_DATA_DELAY_MS 260U
#define FLRC_VIDEO_TX_REPAIR_DELAY_MS 300U
#define FLRC_VIDEO_FIRST_DATA_REPEAT 6U
#define FLRC_VIDEO_DATA_REPEAT 2U
#define FLRC_VIDEO_REPAIR_REPEAT 4U
#define FLRC_VIDEO_MAX_RETRIES_DEFAULT 10U
#define FLRC_VIDEO_ACK_WAIT_MS_DEFAULT 5000U
#define FLRC_VIDEO_FAST_START_REPEAT 6U
#define FLRC_VIDEO_FAST_END_REPEAT 4U
#define FLRC_VIDEO_FAST_FIRST_DATA_REPEAT 3U
#define FLRC_VIDEO_FAST_DATA_REPEAT 2U
#define FLRC_VIDEO_FAST_REPAIR_REPEAT 5U
#define FLRC_VIDEO_FAST_REPAIR_ROUNDS 3U
#define FLRC_VIDEO_FAST_ACK_WAIT_MS 180U
#define FLRC_VIDEO_FAST_CTRL_REPEAT 10U
#define FLRC_VIDEO_FAST_CTRL_REPLY_DELAY_MS 20U
#define FLRC_VIDEO_FAST_FRAME_GAP_US 2500U
#define FLRC_STREAM2_MAGIC 0x3253464BU
#define FLRC_STREAM2_VERSION 1U
#define FLRC_STREAM2_HDR_LEN 32U
#define FLRC_STREAM2_FLAG_FIRST 0x01U
#define FLRC_STREAM2_FLAG_LAST 0x02U
#define FLRC_STREAM2_META_PATH "/tmp/k230_flrc_camera_preview.meta"
#define FLRC_STREAM2_JPEG_WIDTH_DEFAULT 120U
#define FLRC_STREAM2_JPEG_HEIGHT_DEFAULT 90U

enum {
    K230_HAL_GPIO_INPUT = 0,
    K230_HAL_GPIO_OUTPUT = 1,
    K230_HAL_GPIO_LOW = 0,
    K230_HAL_GPIO_HIGH = 1,
    K230_HAL_GPIO_RISING = 1,
    K230_HAL_GPIO_FALLING = 2,
};

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

class K230BenchHal : public RadioLibHal {
public:
    K230BenchHal(const char *spi_path, uint32_t spi_speed)
        : RadioLibHal(K230_HAL_GPIO_INPUT, K230_HAL_GPIO_OUTPUT,
                      K230_HAL_GPIO_LOW, K230_HAL_GPIO_HIGH,
                      K230_HAL_GPIO_RISING, K230_HAL_GPIO_FALLING),
          spi_path_(spi_path), spi_speed_(spi_speed)
    {
        memset(gpio_lines_, 0, sizeof(gpio_lines_));
    }

    ~K230BenchHal() override
    {
        term();
    }

    void init() override
    {
        spiBegin();
    }

    void term() override
    {
        spiEnd();
        for(size_t i = 0; i < 64U; i++) {
            release_pin((uint32_t)i);
        }
        for(size_t i = 0; i < 2U; i++) {
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
        (void)interrupt_num;
        (void)interrupt_cb;
        (void)mode;
    }

    void detachInterrupt(uint32_t interrupt_num) override
    {
        (void)interrupt_num;
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
        return (RadioLibTime_t)monotonic_us();
    }

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override
    {
        RadioLibTime_t start = micros();
        while((micros() - start) < timeout) {
            if(digitalRead(pin) == state) {
                RadioLibTime_t pulse_start = micros();
                while((micros() - start) < timeout &&
                      digitalRead(pin) == state) {
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
            set_error("%s open failed: %s", spi_path_, strerror(errno));
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
    } gpio_line_t;

    const char *spi_path_;
    uint32_t spi_speed_;
    int spi_fd_ = -1;
    struct gpiod_chip *chips_[2] = {nullptr, nullptr};
    gpio_line_t gpio_lines_[64];
    char last_error_[160] = "";

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

    void release_pin(uint32_t pin)
    {
        if(pin >= 64U || !gpio_lines_[pin].request) {
            return;
        }
        gpiod_line_request_release(gpio_lines_[pin].request);
        gpio_lines_[pin].request = nullptr;
        gpio_lines_[pin].active = 0;
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
                                         "k230-lora-flrc-bench");

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
        return 1;
    }
};

typedef struct {
    const char *role;
    const char *file_path;
    const char *in_dir;
    const char *out_dir;
    float freq_mhz;
    unsigned bitrate_kbps;
    unsigned duration_s;
    unsigned payload_len;
    unsigned spi_hz;
    int power_dbm;
    unsigned rx_poll_us;
    unsigned max_retries;
    unsigned ack_wait_ms;
    int keep_listening;
    int fast_frame;
    unsigned frame_width;
    unsigned frame_height;
    unsigned frame_gap_us;
} bench_config_t;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t errors;
    uint64_t dropped_queue_files;
    uint64_t last_log_us;
    int16_t last_state;
} bench_stats_t;

typedef struct {
    uint8_t type;
    uint16_t payload_len;
    uint32_t session;
    uint32_t seq;
    uint32_t total_size;
    uint32_t crc32;
} flrc_video_header_t;

typedef struct {
    uint16_t payload_len;
    uint8_t flags;
    uint32_t session;
    uint32_t frame_id;
    uint16_t chunk_id;
    uint16_t chunk_count;
    uint32_t frame_size;
    uint32_t frame_crc;
    uint32_t payload_crc;
} flrc_stream2_header_t;

#ifdef K230_FLRC_TILE_STREAM
#define FLRC_TILE_MAGIC 0x544C464BU
#define FLRC_TILE_VERSION 1U
#define FLRC_TILE_HDR_LEN 32U
#define FLRC_TILE_WIDTH_DEFAULT 20U
#define FLRC_TILE_HEIGHT_DEFAULT 5U
#define FLRC_TILE_CANVAS_W_DEFAULT 320U
#define FLRC_TILE_CANVAS_H_DEFAULT 240U
#define FLRC_TILE_REPEAT_DEFAULT 2U
#define FLRC_TILE_GAP_US_DEFAULT 1200U
#define FLRC_TILE_FRAME_GAP_US_DEFAULT 5000U
#define FLRC_TILE_PREVIEW_PATH_DEFAULT "/tmp/k230_flrc_camera_preview.rgb565"

typedef struct {
    const char *role;
    const char *frame_path;
    const char *in_dir;
    const char *preview_path;
    float freq_mhz;
    unsigned bitrate_kbps;
    unsigned duration_s;
    unsigned payload_len;
    unsigned spi_hz;
    int power_dbm;
    unsigned rx_poll_us;
    unsigned width;
    unsigned height;
    unsigned tile_w;
    unsigned tile_h;
    unsigned repeat;
    unsigned tile_gap_us;
    unsigned frame_gap_us;
} tile_config_t;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t errors;
    uint64_t frames;
    uint64_t tiles;
    uint64_t bad_crc;
    uint64_t duplicate_tiles;
    uint64_t dropped_queue_frames;
    uint64_t last_log_us;
    int16_t last_state;
} tile_stats_t;

typedef struct {
    uint16_t payload_len;
    uint32_t session;
    uint32_t frame_id;
    uint16_t tile_id;
    uint16_t tile_count;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint32_t crc32;
} flrc_tile_header_t;
#endif

static const uint32_t lr2021_lilygo_rf_switch_dio_pins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LR2021_DIO6, RADIOLIB_LR2021_DIO7,
    RADIOLIB_LR2021_DIO8, RADIOLIB_LR2021_DIO10,
    RADIOLIB_NC,
};

static const Module::RfSwitchMode_t lr2021_lilygo_rf_switch_table[] = {
    // mode                  DIO6              DIO7              DIO8               DIO10
    {LR2021::MODE_STBY,  {K230_HAL_GPIO_LOW,  K230_HAL_GPIO_LOW, K230_HAL_GPIO_LOW,  K230_HAL_GPIO_LOW, 0}},
    {LR2021::MODE_RX,    {K230_HAL_GPIO_LOW,  K230_HAL_GPIO_LOW, K230_HAL_GPIO_HIGH, K230_HAL_GPIO_LOW, 0}},
    {LR2021::MODE_TX,    {K230_HAL_GPIO_LOW,  K230_HAL_GPIO_LOW, K230_HAL_GPIO_HIGH, K230_HAL_GPIO_LOW, 0}},
    {LR2021::MODE_RX_HF, {K230_HAL_GPIO_HIGH, K230_HAL_GPIO_LOW, K230_HAL_GPIO_LOW,  K230_HAL_GPIO_HIGH, 0}},
    {LR2021::MODE_TX_HF, {K230_HAL_GPIO_LOW,  K230_HAL_GPIO_HIGH, K230_HAL_GPIO_LOW, K230_HAL_GPIO_HIGH, 0}},
    END_OF_MODE_TABLE,
};

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

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = crc ^ 0xFFFFFFFFU;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8U; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static int file_crc32(const char *path, uint32_t *crc_out, uint32_t *size_out)
{
    FILE *fp = fopen(path, "rb");
    uint8_t buf[4096];
    uint32_t crc = 0;
    uint32_t total = 0;

    if(!fp) {
        return -1;
    }
    for(;;) {
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if(got > 0) {
            crc = crc32_update(crc, buf, got);
            total += (uint32_t)got;
        }
        if(got < sizeof(buf)) {
            if(ferror(fp)) {
                fclose(fp);
                return -1;
            }
            break;
        }
    }
    fclose(fp);
    if(crc_out) {
        *crc_out = crc;
    }
    if(size_out) {
        *size_out = total;
    }
    return 0;
}

static const char *path_basename(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;

    return slash ? slash + 1 : (path ? path : "video.bin");
}

static void sanitize_filename(const char *src, char *dst, size_t dst_len)
{
    size_t used = 0;

    if(!dst || dst_len == 0U) {
        return;
    }
    if(!src || !src[0]) {
        src = "flrc_video.bin";
    }
    for(const char *p = src; *p && used + 1U < dst_len; p++) {
        char c = *p;
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dst[used++] = c;
        } else {
            dst[used++] = '_';
        }
    }
    dst[used] = '\0';
    if(!dst[0]) {
        snprintf(dst, dst_len, "flrc_video.bin");
    }
}

static int flrc_video_name_payload_valid(const uint8_t *payload,
                                         uint16_t payload_len)
{
    if(!payload || payload_len == 0U || payload_len > 100U) {
        return 0;
    }
    for(uint16_t i = 0; i < payload_len; i++) {
        uint8_t c = payload[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int flrc_video_name_char_valid(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static int flrc_video_sanitize_name_payload(const uint8_t *payload,
                                            uint16_t payload_len,
                                            char *dst, size_t dst_len,
                                            uint32_t session)
{
    size_t used = 0;
    int repaired = 0;
    int has_dot = 0;

    if(!dst || dst_len == 0U) {
        return 0;
    }
    dst[0] = '\0';
    if(!payload || payload_len == 0U || payload_len > 100U) {
        snprintf(dst, dst_len, "flrc_rx_%08x.bin", session);
        return 0;
    }
    for(uint16_t i = 0; i < payload_len && used + 1U < dst_len; i++) {
        uint8_t c = payload[i];

        if(c == '\0') {
            break;
        }
        if(flrc_video_name_char_valid(c)) {
            dst[used++] = (char)c;
            if(c == '.') {
                has_dot = 1;
            }
        } else {
            dst[used++] = '_';
            repaired = 1;
        }
    }
    dst[used] = '\0';
    if(used == 0U) {
        snprintf(dst, dst_len, "flrc_rx_%08x.bin", session);
        return 0;
    }
    if(!has_dot && used + 4U < dst_len) {
        snprintf(dst + used, dst_len - used, ".bin");
        repaired = 1;
    }
    sanitize_filename(dst, dst, dst_len);
    return !repaired;
}

static void flrc_video_make_packet(uint8_t *packet, unsigned packet_len,
                                   uint8_t type, uint32_t session,
                                   uint32_t seq, uint32_t total_size,
                                   uint32_t crc32, const uint8_t *payload,
                                   uint16_t payload_len)
{
    memset(packet, 0, packet_len);
    put_le32(packet + 0, FLRC_VIDEO_MAGIC);
    packet[4] = FLRC_VIDEO_VERSION;
    packet[5] = type;
    put_le16(packet + 6, payload_len);
    put_le32(packet + 8, session);
    put_le32(packet + 12, seq);
    put_le32(packet + 16, total_size);
    put_le32(packet + 20, crc32);
    if(payload && payload_len > 0U && FLRC_VIDEO_HDR_LEN + payload_len <= packet_len) {
        memcpy(packet + FLRC_VIDEO_HDR_LEN, payload, payload_len);
    }
}

static int flrc_video_parse_packet(const uint8_t *packet, unsigned packet_len,
                                   flrc_video_header_t *header)
{
    if(!packet || !header || packet_len < FLRC_VIDEO_HDR_LEN) {
        return -1;
    }
    if(get_le32(packet + 0) != FLRC_VIDEO_MAGIC ||
       packet[4] != FLRC_VIDEO_VERSION) {
        return -1;
    }
    header->type = packet[5];
    header->payload_len = get_le16(packet + 6);
    header->session = get_le32(packet + 8);
    header->seq = get_le32(packet + 12);
    header->total_size = get_le32(packet + 16);
    header->crc32 = get_le32(packet + 20);
    if(header->payload_len > packet_len - FLRC_VIDEO_HDR_LEN) {
        return -1;
    }
    if(header->type < FLRC_VIDEO_TYPE_START ||
       header->type > FLRC_VIDEO_TYPE_DONE) {
        return -1;
    }
    return 0;
}

static unsigned flrc_video_nack_capacity(unsigned packet_len)
{
    if(packet_len <= FLRC_VIDEO_HDR_LEN + 2U) {
        return 0;
    }
    return (packet_len - FLRC_VIDEO_HDR_LEN - 2U) / 4U;
}

static unsigned flrc_video_collect_missing(const uint8_t *seen,
                                           uint32_t expected_chunks,
                                           uint32_t *seqs,
                                           unsigned max_seqs)
{
    unsigned count = 0;

    if(!seen || !seqs || max_seqs == 0U) {
        return 0;
    }
    for(uint32_t seq = 0; seq < expected_chunks && count < max_seqs; seq++) {
        if(!seen[seq]) {
            seqs[count++] = seq;
        }
    }
    return count;
}

static void flrc_video_make_nack_packet(uint8_t *packet, unsigned packet_len,
                                        uint32_t session, uint32_t round,
                                        uint32_t total_size,
                                        uint32_t file_crc,
                                        const uint32_t *seqs,
                                        unsigned count)
{
    uint8_t payload[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    unsigned cap = flrc_video_nack_capacity(packet_len);

    if(count > cap) {
        count = cap;
    }
    memset(payload, 0, sizeof(payload));
    put_le16(payload, (uint16_t)count);
    for(unsigned i = 0; i < count; i++) {
        put_le32(payload + 2U + i * 4U, seqs[i]);
    }
    flrc_video_make_packet(packet, packet_len, FLRC_VIDEO_TYPE_NACK,
                           session, round, total_size, file_crc, payload,
                           (uint16_t)(2U + count * 4U));
}

static int flrc_video_parse_nack(const uint8_t *packet,
                                 const flrc_video_header_t *hdr,
                                 uint32_t *seqs, unsigned max_seqs,
                                 unsigned *count_out)
{
    unsigned count;

    if(!packet || !hdr || hdr->type != FLRC_VIDEO_TYPE_NACK ||
       hdr->payload_len < 2U || !count_out) {
        return -1;
    }
    count = get_le16(packet + FLRC_VIDEO_HDR_LEN);
    if(count > max_seqs ||
       hdr->payload_len < 2U + count * 4U) {
        return -1;
    }
    for(unsigned i = 0; i < count; i++) {
        seqs[i] = get_le32(packet + FLRC_VIDEO_HDR_LEN + 2U + i * 4U);
    }
    *count_out = count;
    return 0;
}

static void flrc_stream2_make_packet(uint8_t *packet, unsigned packet_len,
                                     uint32_t session, uint32_t frame_id,
                                     uint16_t chunk_id,
                                     uint16_t chunk_count,
                                     uint8_t flags,
                                     uint32_t frame_size,
                                     uint32_t frame_crc,
                                     const uint8_t *payload,
                                     uint16_t payload_len)
{
    memset(packet, 0, packet_len);
    put_le32(packet + 0, FLRC_STREAM2_MAGIC);
    packet[4] = FLRC_STREAM2_VERSION;
    packet[5] = flags;
    put_le16(packet + 6, payload_len);
    put_le32(packet + 8, session);
    put_le32(packet + 12, frame_id);
    put_le16(packet + 16, chunk_id);
    put_le16(packet + 18, chunk_count);
    put_le32(packet + 20, frame_size);
    put_le32(packet + 24, frame_crc);
    put_le32(packet + 28, crc32_update(0, payload, payload_len));
    if(payload && payload_len > 0U &&
       FLRC_STREAM2_HDR_LEN + payload_len <= packet_len) {
        memcpy(packet + FLRC_STREAM2_HDR_LEN, payload, payload_len);
    }
}

static int flrc_stream2_parse_packet(const uint8_t *packet, unsigned packet_len,
                                     flrc_stream2_header_t *hdr)
{
    if(!packet || !hdr || packet_len < FLRC_STREAM2_HDR_LEN) {
        return -1;
    }
    if(get_le32(packet + 0) != FLRC_STREAM2_MAGIC ||
       packet[4] != FLRC_STREAM2_VERSION) {
        return -1;
    }
    hdr->flags = packet[5];
    hdr->payload_len = get_le16(packet + 6);
    hdr->session = get_le32(packet + 8);
    hdr->frame_id = get_le32(packet + 12);
    hdr->chunk_id = get_le16(packet + 16);
    hdr->chunk_count = get_le16(packet + 18);
    hdr->frame_size = get_le32(packet + 20);
    hdr->frame_crc = get_le32(packet + 24);
    hdr->payload_crc = get_le32(packet + 28);
    if(hdr->payload_len > packet_len - FLRC_STREAM2_HDR_LEN ||
       hdr->chunk_count == 0U || hdr->chunk_id >= hdr->chunk_count ||
       hdr->frame_size == 0U) {
        return -1;
    }
    if(crc32_update(0, packet + FLRC_STREAM2_HDR_LEN,
                    hdr->payload_len) != hdr->payload_crc) {
        return -1;
    }
    return 0;
}

static int should_retry_xtal(int16_t state)
{
    return state == RADIOLIB_ERR_SPI_CMD_INVALID ||
           state == RADIOLIB_ERR_SPI_CMD_FAILED;
}

static int16_t set_lilygo_hf_power(LR2021 &radio, int power)
{
    int safe_power = power;
    if(safe_power < -19) {
        safe_power = -19;
    }
    if(safe_power > 4) {
        safe_power = 4;
    }
    int16_t state = radio.setOutputPower((int8_t)safe_power);
    if(state == RADIOLIB_ERR_SPI_CMD_INVALID) {
        return RADIOLIB_ERR_NONE;
    }
    return state;
}

static int16_t begin_flrc(LR2021 &radio, const bench_config_t &cfg)
{
    uint8_t flrc_sync[] = {0x2D, 0x01, 0x4B, 0x1D};
    int16_t state;

    radio.irqDioNum = BENCH_LR2021_IRQ_DIO_NUM;
    state = radio.beginFLRC(cfg.freq_mhz, (uint16_t)cfg.bitrate_kbps,
                            RADIOLIB_LR2021_FLRC_CR_3_4,
                            (int8_t)cfg.power_dbm, 16,
                            RADIOLIB_SHAPING_0_5, 3.0f);
    printf("beginFLRC freq=%.1f br=%u power=%d tcxo=3.0 state=%d %s\n",
           cfg.freq_mhz, cfg.bitrate_kbps, cfg.power_dbm, state,
           error_name(state));
    if(should_retry_xtal(state)) {
        state = radio.beginFLRC(cfg.freq_mhz, (uint16_t)cfg.bitrate_kbps,
                                RADIOLIB_LR2021_FLRC_CR_3_4,
                                (int8_t)cfg.power_dbm, 16,
                                RADIOLIB_SHAPING_0_5, 0.0f);
        printf("beginFLRC retry XTAL tcxo=0 state=%d %s\n", state,
               error_name(state));
    }
    if(state != RADIOLIB_ERR_NONE &&
       state != RADIOLIB_ERR_SPI_CMD_INVALID) {
        return state;
    }

    radio.setRfSwitchTable(lr2021_lilygo_rf_switch_dio_pins,
                           lr2021_lilygo_rf_switch_table);
    state = set_lilygo_hf_power(radio, cfg.power_dbm);
    printf("setOutputPower safe<=4 requested=%d state=%d %s\n",
           cfg.power_dbm, state, error_name(state));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }

    state = radio.setPreambleLength(16);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = radio.setSyncWord(flrc_sync, sizeof(flrc_sync));
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = radio.fixedPacketLengthMode((uint8_t)cfg.payload_len);
    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    state = radio.setCRC(2);
    return state;
}

static int16_t fast_transmit(K230BenchHal &hal, LR2021 &radio,
                             uint8_t *payload, size_t len,
                             unsigned bitrate_kbps)
{
    int16_t state = radio.startTransmit(payload, len);
    uint64_t start_us;
    uint64_t timeout_us;

    if(state != RADIOLIB_ERR_NONE) {
        return state;
    }
    timeout_us = ((uint64_t)len * 8ULL * 1000ULL) /
                 (uint64_t)(bitrate_kbps ? bitrate_kbps :
                            BENCH_BR_DEFAULT);
    timeout_us = timeout_us * 6ULL + 20000ULL;
    if(timeout_us < 30000ULL) {
        timeout_us = 30000ULL;
    }

    start_us = monotonic_us();
    while(!hal.digitalRead(BENCH_PIN_DIO1)) {
        if(monotonic_us() - start_us > timeout_us) {
            (void)radio.finishTransmit();
            return RADIOLIB_ERR_TX_TIMEOUT;
        }
        sched_yield();
    }
    return radio.finishTransmit();
}

static int16_t start_continuous_rx(LR2021 &radio, unsigned payload_len)
{
    return radio.startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF,
                              RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
                              RADIOLIB_IRQ_RX_DEFAULT_MASK,
                              payload_len);
}

static int16_t restart_continuous_rx(K230BenchHal &hal, LR2021 &radio,
                                     const bench_config_t &cfg,
                                     const char *tag)
{
    int16_t state = RADIOLIB_ERR_NONE;

    for(unsigned attempt = 1; attempt <= 4U; attempt++) {
        state = start_continuous_rx(radio, cfg.payload_len);
        if(state == RADIOLIB_ERR_NONE) {
            return state;
        }
        printf("%s rx restart attempt=%u state=%d %s\n",
               tag ? tag : "RX", attempt, state, error_name(state));
        (void)radio.standby();
        hal.delay(20U * attempt);
    }

    printf("%s rx restart reinit after state=%d %s\n",
           tag ? tag : "RX", state, error_name(state));
    state = begin_flrc(radio, cfg);
    if(state != RADIOLIB_ERR_NONE) {
        printf("%s rx restart reinit failed state=%d %s\n",
               tag ? tag : "RX", state, error_name(state));
        return state;
    }
    hal.delay(40);
    for(unsigned attempt = 1; attempt <= 4U; attempt++) {
        state = start_continuous_rx(radio, cfg.payload_len);
        if(state == RADIOLIB_ERR_NONE) {
            return state;
        }
        printf("%s rx restart post-reinit attempt=%u state=%d %s\n",
               tag ? tag : "RX", attempt, state, error_name(state));
        (void)radio.standby();
        hal.delay(30U * attempt);
    }
    return state;
}

static void print_stats(const char *tag, const bench_config_t &cfg,
                        const bench_stats_t &stats, uint64_t start_us)
{
    uint64_t elapsed_us = monotonic_us() - start_us;
    double mbps = 0.0;

    if(elapsed_us > 0) {
        mbps = ((double)stats.bytes * 8.0) / (double)elapsed_us;
    }
    printf("%s role=%s mbps=%.4f packets=%llu bytes=%llu errors=%llu last=%d freq=%.1f br=%u len=%u spi=%u power=%d elapsed_ms=%llu\n",
           tag, cfg.role, mbps, (unsigned long long)stats.packets,
           (unsigned long long)stats.bytes,
           (unsigned long long)stats.errors, stats.last_state,
           cfg.freq_mhz, cfg.bitrate_kbps, cfg.payload_len, cfg.spi_hz,
           cfg.power_dbm, (unsigned long long)(elapsed_us / 1000ULL));
    fflush(stdout);
}

static int run_tx(K230BenchHal &hal, LR2021 &radio, const bench_config_t &cfg)
{
    bench_stats_t stats = {};
    uint8_t payload[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    uint32_t seq = 0;

    memset(payload, 0xA5, sizeof(payload));
    while(monotonic_us() < end_us) {
        int16_t state;

        seq++;
        memcpy(payload, &seq, sizeof(seq));
        state = fast_transmit(hal, radio, payload, cfg.payload_len,
                              cfg.bitrate_kbps);
        stats.last_state = state;
        if(state == RADIOLIB_ERR_NONE) {
            stats.packets++;
            stats.bytes += cfg.payload_len;
        } else {
            stats.errors++;
            if(stats.errors < 10 || (stats.errors % 100ULL) == 0ULL) {
                printf("TX state=%d %s\n", state, error_name(state));
            }
            hal.delay(2);
        }
        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            stats.last_log_us = monotonic_us();
            print_stats("STATS", cfg, stats, start_us);
        }
    }
    (void)radio.standby();
    print_stats("RESULT", cfg, stats, start_us);
    return 0;
}

static int run_rx(K230BenchHal &hal, LR2021 &radio, const bench_config_t &cfg)
{
    bench_stats_t stats = {};
    uint8_t payload[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    int16_t state;

    state = start_continuous_rx(radio, cfg.payload_len);
    if(state != RADIOLIB_ERR_NONE) {
        printf("RX start state=%d %s\n", state, error_name(state));
        return 2;
    }

    while(monotonic_us() < end_us) {
        if(!hal.digitalRead(BENCH_PIN_DIO1)) {
            usleep((useconds_t)cfg.rx_poll_us);
        } else {
            state = radio.readData(payload, cfg.payload_len);
            stats.last_state = state;
            if(state == RADIOLIB_ERR_NONE) {
                stats.packets++;
                stats.bytes += cfg.payload_len;
            } else {
                stats.errors++;
                if(stats.errors < 10 || (stats.errors % 100ULL) == 0ULL) {
                    printf("RX state=%d %s\n", state, error_name(state));
                }
                (void)radio.finishReceive();
                hal.delayMicroseconds(250);
                state = start_continuous_rx(radio, cfg.payload_len);
                if(state != RADIOLIB_ERR_NONE) {
                    stats.errors++;
                    stats.last_state = state;
                    printf("RX restart state=%d %s\n", state,
                           error_name(state));
                    break;
                }
            }
        }

        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            stats.last_log_us = monotonic_us();
            print_stats("STATS", cfg, stats, start_us);
        }
    }
    (void)radio.standby();
    print_stats("RESULT", cfg, stats, start_us);
    return 0;
}

static int flrc_video_tx_account(K230BenchHal &hal, LR2021 &radio,
                                 const bench_config_t &cfg,
                                 bench_stats_t *stats,
                                 uint8_t *packet)
{
    int16_t state = fast_transmit(hal, radio, packet, cfg.payload_len,
                                  cfg.bitrate_kbps);

    stats->last_state = state;
    if(state == RADIOLIB_ERR_NONE) {
        stats->packets++;
        stats->bytes += cfg.payload_len;
    } else {
        stats->errors++;
    }
    return state;
}

static int flrc_video_send_start(K230BenchHal &hal, LR2021 &radio,
                                 const bench_config_t &cfg,
                                 bench_stats_t *stats, uint8_t *packet,
                                 uint32_t session, uint32_t file_size,
                                 uint32_t file_crc, const char *name,
                                 unsigned chunk_len)
{
    uint16_t name_len = (uint16_t)strlen(name);
    int rc = 0;
    unsigned repeat = cfg.fast_frame ? FLRC_VIDEO_FAST_START_REPEAT :
                      FLRC_VIDEO_START_REPEAT;
    unsigned gap_us = cfg.fast_frame ? FLRC_VIDEO_FAST_FRAME_GAP_US :
                      FLRC_VIDEO_REPEAT_GAP_US;

    if(name_len > chunk_len) {
        name_len = (uint16_t)chunk_len;
    }
    for(unsigned i = 0; i < repeat; i++) {
        flrc_video_make_packet(packet, cfg.payload_len, FLRC_VIDEO_TYPE_START,
                               session, 0, file_size, file_crc,
                               (const uint8_t *)name, name_len);
        if(flrc_video_tx_account(hal, radio, cfg, stats, packet) !=
           RADIOLIB_ERR_NONE) {
            rc = 1;
        }
        if(i + 1U < repeat) {
            hal.delayMicroseconds(gap_us);
        }
    }
    return rc;
}

static int flrc_video_send_end(K230BenchHal &hal, LR2021 &radio,
                               const bench_config_t &cfg,
                               bench_stats_t *stats, uint8_t *packet,
                               uint32_t session, uint32_t end_seq,
                               uint32_t file_size, uint32_t file_crc)
{
    int rc = 0;
    unsigned repeat = cfg.fast_frame ? FLRC_VIDEO_FAST_END_REPEAT :
                      FLRC_VIDEO_END_REPEAT;
    unsigned gap_us = cfg.fast_frame ? FLRC_VIDEO_FAST_FRAME_GAP_US :
                      FLRC_VIDEO_REPEAT_GAP_US;

    for(unsigned i = 0; i < repeat; i++) {
        flrc_video_make_packet(packet, cfg.payload_len, FLRC_VIDEO_TYPE_END,
                               session, end_seq, file_size, file_crc, NULL, 0);
        if(flrc_video_tx_account(hal, radio, cfg, stats, packet) !=
           RADIOLIB_ERR_NONE) {
            rc = 1;
        }
        if(i + 1U < repeat) {
            hal.delayMicroseconds(gap_us);
        }
    }
    return rc;
}

static int flrc_video_send_done(K230BenchHal &hal, LR2021 &radio,
                                const bench_config_t &cfg,
                                bench_stats_t *stats, uint8_t *packet,
                                uint32_t session, uint32_t round,
                                uint32_t file_size, uint32_t file_crc)
{
    int rc = 0;
    unsigned repeat = cfg.fast_frame ? FLRC_VIDEO_FAST_CTRL_REPEAT :
                      FLRC_VIDEO_CTRL_REPEAT;
    unsigned gap_us = cfg.fast_frame ? FLRC_VIDEO_FAST_FRAME_GAP_US :
                      FLRC_VIDEO_REPEAT_GAP_US;

    for(unsigned i = 0; i < repeat; i++) {
        flrc_video_make_packet(packet, cfg.payload_len, FLRC_VIDEO_TYPE_DONE,
                               session, round, file_size, file_crc, NULL, 0);
        if(flrc_video_tx_account(hal, radio, cfg, stats, packet) !=
           RADIOLIB_ERR_NONE) {
            rc = 1;
        }
        if(i + 1U < repeat) {
            hal.delayMicroseconds(gap_us);
        }
    }
    return rc;
}

static int flrc_video_send_nack(K230BenchHal &hal, LR2021 &radio,
                                const bench_config_t &cfg,
                                bench_stats_t *stats, uint8_t *packet,
                                uint32_t session, uint32_t round,
                                uint32_t file_size, uint32_t file_crc,
                                const uint32_t *seqs, unsigned count)
{
    int rc = 0;
    unsigned repeat = cfg.fast_frame ? FLRC_VIDEO_FAST_CTRL_REPEAT :
                      FLRC_VIDEO_CTRL_REPEAT;
    unsigned gap_us = cfg.fast_frame ? FLRC_VIDEO_FAST_FRAME_GAP_US :
                      FLRC_VIDEO_REPEAT_GAP_US;

    for(unsigned i = 0; i < repeat; i++) {
        flrc_video_make_nack_packet(packet, cfg.payload_len, session, round,
                                    file_size, file_crc, seqs, count);
        if(flrc_video_tx_account(hal, radio, cfg, stats, packet) !=
           RADIOLIB_ERR_NONE) {
            rc = 1;
        }
        if(i + 1U < repeat) {
            hal.delayMicroseconds(gap_us);
        }
    }
    return rc;
}

static int flrc_video_send_data_seq(K230BenchHal &hal, LR2021 &radio,
                                    const bench_config_t &cfg,
                                    bench_stats_t *stats, FILE *fp,
                                    uint8_t *packet, uint8_t *chunk,
                                    uint32_t session, uint32_t seq,
                                    uint32_t file_size, unsigned chunk_len)
{
    uint64_t offset = (uint64_t)seq * (uint64_t)chunk_len;
    size_t need;
    size_t got;
    uint32_t payload_crc;
    int16_t state;

    if(offset >= file_size) {
        return -1;
    }
    need = chunk_len;
    if(offset + need > file_size) {
        need = (size_t)(file_size - offset);
    }
    if(fseek(fp, (long)offset, SEEK_SET) != 0) {
        stats->errors++;
        return -1;
    }
    got = fread(chunk, 1, need, fp);
    if(got != need) {
        stats->errors++;
        return -1;
    }
    payload_crc = crc32_update(0, chunk, got);
    flrc_video_make_packet(packet, cfg.payload_len, FLRC_VIDEO_TYPE_DATA,
                           session, seq, file_size, payload_crc, chunk,
                           (uint16_t)got);
    state = flrc_video_tx_account(hal, radio, cfg, stats, packet);
    if(state != RADIOLIB_ERR_NONE) {
        printf("FILE_TX data seq=%u state=%d %s\n", seq, state,
               error_name(state));
        return -1;
    }
    return 0;
}

static int flrc_video_wait_control(K230BenchHal &hal, LR2021 &radio,
                                   const bench_config_t &cfg,
                                   uint32_t session, uint32_t file_size,
                                   uint32_t file_crc, uint32_t *seqs,
                                   unsigned max_seqs, unsigned *count_out,
                                   int *done_out)
{
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint64_t end_us;
    int16_t state;

    if(count_out) {
        *count_out = 0;
    }
    if(done_out) {
        *done_out = 0;
    }
    state = restart_continuous_rx(hal, radio, cfg, "FILE_TX_CONTROL");
    if(state != RADIOLIB_ERR_NONE) {
        printf("FILE_TX control rx start state=%d %s\n", state,
               error_name(state));
        return -1;
    }
    end_us = monotonic_us() + (uint64_t)cfg.ack_wait_ms * 1000ULL;
    while(monotonic_us() < end_us) {
        flrc_video_header_t hdr;

        if(!hal.digitalRead(BENCH_PIN_DIO1)) {
            usleep((useconds_t)cfg.rx_poll_us);
            continue;
        }
        state = radio.readData(packet, cfg.payload_len);
        if(state != RADIOLIB_ERR_NONE) {
            (void)radio.finishReceive();
            hal.delayMicroseconds(250);
            (void)restart_continuous_rx(hal, radio, cfg, "FILE_TX_CONTROL");
            continue;
        }
        if(flrc_video_parse_packet(packet, cfg.payload_len, &hdr) != 0 ||
           hdr.session != session || hdr.total_size != file_size ||
           hdr.crc32 != file_crc) {
            continue;
        }
        if(hdr.type == FLRC_VIDEO_TYPE_DONE) {
            if(done_out) {
                *done_out = 1;
            }
            return 1;
        }
        if(hdr.type == FLRC_VIDEO_TYPE_NACK &&
           flrc_video_parse_nack(packet, &hdr, seqs, max_seqs,
                                 count_out) == 0) {
            return 2;
        }
    }
    return 0;
}

static int run_file_tx(K230BenchHal &hal, LR2021 &radio,
                       const bench_config_t &cfg)
{
    bench_stats_t stats = {};
    FILE *fp;
    struct stat st;
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint8_t chunk[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint32_t nack_seqs[RADIOLIB_LR2021_MAX_PACKET_LENGTH / 4U];
    uint32_t file_crc = 0;
    uint32_t file_size = 0;
    uint32_t session = (uint32_t)monotonic_us();
    uint32_t expected_chunks = 0;
    unsigned chunk_len = cfg.payload_len - FLRC_VIDEO_HDR_LEN;
    unsigned nack_cap = flrc_video_nack_capacity(cfg.payload_len);
    uint64_t start_us;
    const char *name;
    unsigned retry_round = 0;
    int done = 0;

    if(!cfg.file_path || !cfg.file_path[0]) {
        fprintf(stderr, "file-tx requires --file\n");
        return 2;
    }
    if(stat(cfg.file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "open input stat failed: %s\n", strerror(errno));
        return 2;
    }
    if(st.st_size <= 0 || st.st_size > 0xFFFFFFFFLL) {
        fprintf(stderr, "unsupported input size: %lld\n", (long long)st.st_size);
        return 2;
    }
    if(file_crc32(cfg.file_path, &file_crc, &file_size) != 0) {
        fprintf(stderr, "input crc failed: %s\n", strerror(errno));
        return 2;
    }
    fp = fopen(cfg.file_path, "rb");
    if(!fp) {
        fprintf(stderr, "open input failed: %s\n", strerror(errno));
        return 2;
    }

    name = path_basename(cfg.file_path);
    expected_chunks = (file_size + chunk_len - 1U) / chunk_len;
    start_us = monotonic_us();
    printf("FILE_TX_START path=%s name=%s bytes=%u crc=0x%08x chunk=%u chunks=%u session=0x%08x retries=%u ack_wait_ms=%u fast=%d\n",
           cfg.file_path, name, file_size, file_crc, chunk_len,
           expected_chunks, session, cfg.max_retries, cfg.ack_wait_ms,
           cfg.fast_frame);

    (void)flrc_video_send_start(hal, radio, cfg, &stats, packet, session,
                                file_size, file_crc, name, chunk_len);
    if(cfg.fast_frame) {
        hal.delayMicroseconds(FLRC_VIDEO_FAST_FRAME_GAP_US * 4U);
    } else {
        hal.delay(FLRC_VIDEO_TX_DATA_DELAY_MS);
    }

    for(uint32_t seq = 0; seq < expected_chunks; seq++) {
        unsigned repeats;
        unsigned gap_us = cfg.fast_frame ? FLRC_VIDEO_FAST_FRAME_GAP_US :
                          FLRC_VIDEO_REPEAT_GAP_US;

        if(cfg.fast_frame) {
            repeats = (seq == 0U) ? FLRC_VIDEO_FAST_FIRST_DATA_REPEAT :
                      FLRC_VIDEO_FAST_DATA_REPEAT;
        } else {
            repeats = (seq == 0U) ? FLRC_VIDEO_FIRST_DATA_REPEAT :
                      FLRC_VIDEO_DATA_REPEAT;
        }
        for(unsigned r = 0; r < repeats; r++) {
            (void)flrc_video_send_data_seq(hal, radio, cfg, &stats, fp,
                                           packet, chunk, session, seq,
                                           file_size, chunk_len);
            if(cfg.fast_frame) {
                hal.delayMicroseconds(gap_us);
            } else if(r + 1U < repeats) {
                hal.delayMicroseconds(gap_us);
            }
        }
        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            uint64_t elapsed_us = monotonic_us() - start_us;
            double file_mbps = elapsed_us ?
                ((double)((uint64_t)(seq + 1U) * chunk_len) * 8.0) /
                (double)elapsed_us : 0.0;
            stats.last_log_us = monotonic_us();
            printf("STATS role=%s file_mbps=%.4f seq=%u packets=%llu errors=%llu bytes_air=%llu retry_round=%u\n",
                   cfg.role, file_mbps, seq + 1U,
                   (unsigned long long)stats.packets,
                   (unsigned long long)stats.errors,
                   (unsigned long long)stats.bytes, retry_round);
            fflush(stdout);
        }
    }
    (void)flrc_video_send_end(hal, radio, cfg, &stats, packet, session,
                              expected_chunks, file_size, file_crc);

    if(cfg.fast_frame) {
        bench_config_t fast_wait_cfg = cfg;

        fast_wait_cfg.ack_wait_ms = FLRC_VIDEO_FAST_ACK_WAIT_MS;
        while(retry_round < FLRC_VIDEO_FAST_REPAIR_ROUNDS && !done) {
            unsigned nack_count = 0;
            int control;

            retry_round++;
            control = flrc_video_wait_control(hal, radio, fast_wait_cfg,
                                              session, file_size, file_crc,
                                              nack_seqs, nack_cap,
                                              &nack_count, &done);
            if(control == 1 && done) {
                printf("FILE_TX_FAST_DONE_ACK round=%u\n", retry_round);
                break;
            }
            if(control == 2 && nack_count > 0U) {
                printf("FILE_TX_FAST_NACK round=%u count=%u first=%u\n",
                       retry_round, nack_count, nack_seqs[0]);
                for(unsigned i = 0; i < nack_count; i++) {
                    unsigned repeats = FLRC_VIDEO_FAST_REPAIR_REPEAT;

                    if(nack_seqs[i] >= expected_chunks) {
                        continue;
                    }
                    for(unsigned r = 0; r < repeats; r++) {
                        (void)flrc_video_send_data_seq(
                            hal, radio, cfg, &stats, fp, packet, chunk,
                            session, nack_seqs[i], file_size, chunk_len);
                        if(r + 1U < repeats) {
                            hal.delayMicroseconds(
                                FLRC_VIDEO_FAST_FRAME_GAP_US);
                        }
                    }
                }
                (void)flrc_video_send_end(hal, radio, cfg, &stats, packet,
                                          session, expected_chunks, file_size,
                                          file_crc);
            } else {
                printf("FILE_TX_FAST_CONTROL_TIMEOUT round=%u\n",
                       retry_round);
                break;
            }
        }
        fclose(fp);
        (void)radio.standby();
        {
            uint64_t elapsed_us = monotonic_us() - start_us;
            double file_mbps = elapsed_us ?
                ((double)file_size * 8.0) / (double)elapsed_us : 0.0;
            printf("RESULT role=%s fast=1 file_mbps=%.4f file_bytes=%u chunks=%u packets=%llu errors=%llu crc=0x%08x repair_rounds=%u acked=%d elapsed_ms=%llu path=%s\n",
                   cfg.role, file_mbps, file_size, expected_chunks,
                   (unsigned long long)stats.packets,
                   (unsigned long long)stats.errors, file_crc,
                   retry_round, done,
                   (unsigned long long)(elapsed_us / 1000ULL), cfg.file_path);
        }
        fflush(stdout);
        return stats.errors == 0 ? 0 : 1;
    }

    while(retry_round < cfg.max_retries && !done) {
        unsigned nack_count = 0;
        int control;

        retry_round++;
        control = flrc_video_wait_control(hal, radio, cfg, session, file_size,
                                          file_crc, nack_seqs, nack_cap,
                                          &nack_count, &done);
        if(control == 1 && done) {
            printf("FILE_TX_DONE_ACK round=%u\n", retry_round);
            break;
        }
        if(control == 2 && nack_count > 0U) {
            printf("FILE_TX_NACK round=%u count=%u first=%u\n",
                   retry_round, nack_count, nack_seqs[0]);
            hal.delay(FLRC_VIDEO_TX_REPAIR_DELAY_MS);
            for(unsigned i = 0; i < nack_count; i++) {
                unsigned repeats =
                    (nack_seqs[i] == 0U) ? FLRC_VIDEO_FIRST_DATA_REPEAT :
                    FLRC_VIDEO_REPAIR_REPEAT;
                for(unsigned r = 0; r < repeats; r++) {
                    (void)flrc_video_send_data_seq(hal, radio, cfg, &stats, fp,
                                                   packet, chunk, session,
                                                   nack_seqs[i], file_size,
                                                   chunk_len);
                    if(r + 1U < repeats) {
                        hal.delayMicroseconds(FLRC_VIDEO_REPEAT_GAP_US);
                    }
                }
            }
            (void)flrc_video_send_end(hal, radio, cfg, &stats, packet,
                                      session, expected_chunks, file_size,
                                      file_crc);
        } else {
            printf("FILE_TX_CONTROL_TIMEOUT round=%u\n", retry_round);
            (void)flrc_video_send_end(hal, radio, cfg, &stats, packet,
                                      session, expected_chunks, file_size,
                                      file_crc);
        }
    }
    fclose(fp);

    (void)radio.standby();
    {
        uint64_t elapsed_us = monotonic_us() - start_us;
        double file_mbps = elapsed_us ?
            ((double)file_size * 8.0) / (double)elapsed_us : 0.0;
        printf("RESULT role=%s file_mbps=%.4f file_bytes=%u chunks=%u packets=%llu errors=%llu crc=0x%08x retries=%u acked=%d elapsed_ms=%llu path=%s\n",
               cfg.role, file_mbps, file_size, expected_chunks,
               (unsigned long long)stats.packets,
               (unsigned long long)stats.errors, file_crc, retry_round, done,
               (unsigned long long)(elapsed_us / 1000ULL), cfg.file_path);
    }
    fflush(stdout);
    return done ? 0 : 1;
}

static int ends_with(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if(!text || !suffix) {
        return 0;
    }
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

static void ensure_jpeg_filename(char *name, size_t name_len)
{
    const char *dot;
    size_t used;

    if(!name || name_len == 0U || !name[0]) {
        return;
    }
    if(ends_with(name, ".jpg") || ends_with(name, ".jpeg") ||
       ends_with(name, ".JPG") || ends_with(name, ".JPEG")) {
        return;
    }
    dot = strrchr(name, '.');
    if(dot && dot > name) {
        size_t prefix_len = (size_t)(dot - name);
        if(prefix_len + 4U < name_len) {
            memcpy(name + prefix_len, ".jpg", 5U);
            return;
        }
    }
    used = strlen(name);
    if(used + 4U < name_len) {
        memcpy(name + used, ".jpg", 5U);
    }
}

static int find_next_queue_file(const char *dir, char *path, size_t path_len,
                                uint64_t *dropped)
{
    DIR *dp;
    struct dirent *de;
    char best[NAME_MAX + 1] = "";
    std::vector<std::string> stale;

    if(!dir || !dir[0] || !path || path_len == 0U) {
        return 0;
    }
    dp = opendir(dir);
    if(!dp) {
        return 0;
    }
    while((de = readdir(dp)) != NULL) {
        const char *name = de->d_name;

        if(name[0] == '.') {
            continue;
        }
        if(!ends_with(name, ".jpg") && !ends_with(name, ".jpeg") &&
           !ends_with(name, ".bin") && !ends_with(name, ".mp4")) {
            continue;
        }
        if(best[0] != '\0' && strcmp(name, best) < 0) {
            stale.push_back(name);
            continue;
        }
        if(best[0] != '\0') {
            stale.push_back(best);
        }
        if(best[0] == '\0' || strcmp(name, best) > 0) {
            snprintf(best, sizeof(best), "%s", name);
        }
    }
    closedir(dp);
    if(!best[0]) {
        return 0;
    }
    for(const std::string &name : stale) {
        char stale_path[PATH_MAX];

        snprintf(stale_path, sizeof(stale_path), "%s/%s", dir, name.c_str());
        if(unlink(stale_path) == 0 && dropped) {
            (*dropped)++;
        }
    }
    snprintf(path, path_len, "%s/%s", dir, best);
    return 1;
}

static int write_stream2_meta(const char *out_dir, const char *frame_path,
                              unsigned width, unsigned height,
                              uint64_t frames, uint64_t dropped,
                              uint64_t bad_chunks, uint64_t packets,
                              uint64_t bytes, uint64_t start_us)
{
    FILE *fp = fopen(FLRC_STREAM2_META_PATH, "w");
    uint64_t elapsed_us = monotonic_us() - start_us;
    double mbps = elapsed_us ?
        ((double)bytes * 8.0) / (double)elapsed_us : 0.0;

    (void)out_dir;
    if(!fp) {
        return -1;
    }
    fprintf(fp,
            "frame_file=%s\nframes_ready=stream2\nwidth=%u\nheight=%u\nrole=rx\nbuffered=1\nframes=%llu\ndropped=%llu\nbad_chunks=%llu\npackets=%llu\nmbps=%.4f\n",
            frame_path ? frame_path : "",
            width ? width : FLRC_STREAM2_JPEG_WIDTH_DEFAULT,
            height ? height : FLRC_STREAM2_JPEG_HEIGHT_DEFAULT,
            (unsigned long long)frames, (unsigned long long)dropped,
            (unsigned long long)bad_chunks, (unsigned long long)packets,
            mbps);
    fclose(fp);
    return 0;
}

static int run_frame_stream_tx(K230BenchHal &hal, LR2021 &radio,
                               bench_config_t &cfg)
{
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    bench_stats_t stats = {};
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    std::vector<uint8_t> frame;
    uint32_t session = (uint32_t)monotonic_us();
    uint32_t frame_id = 0;
    unsigned chunk_len = cfg.payload_len - FLRC_STREAM2_HDR_LEN;
    uint64_t frames = 0;

    if(!cfg.in_dir || !cfg.in_dir[0]) {
        fprintf(stderr, "frame-stream-tx requires --in-dir\n");
        return 2;
    }
    if(chunk_len == 0U) {
        fprintf(stderr, "payload too small for stream2\n");
        return 2;
    }
    printf("STREAM2_TX_WAIT in_dir=%s duration=%u chunk=%u frame_gap_us=%u width=%u height=%u\n",
           cfg.in_dir, cfg.duration_s, chunk_len, cfg.frame_gap_us,
           cfg.frame_width, cfg.frame_height);

    while(monotonic_us() < end_us) {
        char path[PATH_MAX];
        struct stat st;
        FILE *fp;
        size_t got;
        uint32_t frame_crc;
        uint16_t chunks;

        if(!find_next_queue_file(cfg.in_dir, path, sizeof(path),
                                 &stats.dropped_queue_files)) {
            usleep(10000);
            continue;
        }
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
           st.st_size > 65535) {
            unlink(path);
            stats.errors++;
            continue;
        }
        frame.resize((size_t)st.st_size);
        fp = fopen(path, "rb");
        if(!fp) {
            unlink(path);
            stats.errors++;
            continue;
        }
        got = fread(frame.data(), 1, frame.size(), fp);
        fclose(fp);
        unlink(path);
        if(got != frame.size()) {
            stats.errors++;
            continue;
        }

        frame_id++;
        frame_crc = crc32_update(0, frame.data(), frame.size());
        chunks = (uint16_t)((frame.size() + chunk_len - 1U) / chunk_len);
        for(uint16_t chunk_id = 0; chunk_id < chunks; chunk_id++) {
            size_t offset = (size_t)chunk_id * chunk_len;
            size_t left = frame.size() - offset;
            uint16_t payload_len = (uint16_t)std::min((size_t)chunk_len, left);
            uint8_t flags = 0;
            int16_t state;

            if(chunk_id == 0U) {
                flags |= FLRC_STREAM2_FLAG_FIRST;
            }
            if(chunk_id + 1U == chunks) {
                flags |= FLRC_STREAM2_FLAG_LAST;
            }
            flrc_stream2_make_packet(packet, cfg.payload_len, session,
                                     frame_id, chunk_id, chunks, flags,
                                     (uint32_t)frame.size(), frame_crc,
                                     frame.data() + offset, payload_len);
            state = fast_transmit(hal, radio, packet, cfg.payload_len,
                                  cfg.bitrate_kbps);
            stats.last_state = state;
            if(state == RADIOLIB_ERR_NONE) {
                stats.packets++;
                stats.bytes += cfg.payload_len;
            } else {
                stats.errors++;
            }
            if(cfg.frame_gap_us > 0U) {
                hal.delayMicroseconds(cfg.frame_gap_us);
            }
        }
        frames++;
        printf("STREAM2_TX_FRAME id=%u bytes=%zu chunks=%u dropped=%llu packets=%llu errors=%llu\n",
               frame_id, frame.size(), chunks,
               (unsigned long long)stats.dropped_queue_files,
               (unsigned long long)stats.packets,
               (unsigned long long)stats.errors);
        fflush(stdout);

        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            uint64_t elapsed_us = monotonic_us() - start_us;
            double mbps = elapsed_us ?
                ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;
            stats.last_log_us = monotonic_us();
            printf("STATS role=%s mbps=%.4f packets=%llu bytes=%llu errors=%llu frames=%llu dropped=%llu elapsed_ms=%llu\n",
                   cfg.role, mbps,
                   (unsigned long long)stats.packets,
                   (unsigned long long)stats.bytes,
                   (unsigned long long)stats.errors,
                   (unsigned long long)frames,
                   (unsigned long long)stats.dropped_queue_files,
                   (unsigned long long)((monotonic_us() - start_us) / 1000ULL));
            fflush(stdout);
        }
    }
    (void)radio.standby();
    {
        uint64_t elapsed_us = monotonic_us() - start_us;
        double mbps = elapsed_us ?
            ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;
        printf("RESULT role=%s mbps=%.4f frames=%llu packets=%llu bytes=%llu errors=%llu dropped=%llu elapsed_ms=%llu\n",
               cfg.role, mbps, (unsigned long long)frames,
               (unsigned long long)stats.packets,
               (unsigned long long)stats.bytes,
               (unsigned long long)stats.errors,
               (unsigned long long)stats.dropped_queue_files,
               (unsigned long long)(elapsed_us / 1000ULL));
    }
    return frames > 0U ? 0 : 1;
}

static int run_frame_stream_rx(K230BenchHal &hal, LR2021 &radio,
                               bench_config_t &cfg)
{
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    bench_stats_t stats = {};
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    std::vector<uint8_t> frame;
    std::vector<uint8_t> seen;
    uint32_t current_session = 0;
    uint32_t current_frame = 0;
    uint32_t current_size = 0;
    uint32_t current_crc = 0;
    uint16_t current_chunks = 0;
    uint16_t received_chunks = 0;
    uint64_t completed_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t bad_chunks = 0;
    uint64_t duplicate_chunks = 0;
    int started = 0;
    int16_t state;

    mkdir(cfg.out_dir, 0755);
    state = restart_continuous_rx(hal, radio, cfg, "STREAM2_RX");
    if(state != RADIOLIB_ERR_NONE) {
        printf("STREAM2_RX start state=%d %s\n", state, error_name(state));
        return 2;
    }
    printf("STREAM2_RX_WAIT out_dir=%s duration=%u width=%u height=%u\n",
           cfg.out_dir, cfg.duration_s, cfg.frame_width, cfg.frame_height);

    while(monotonic_us() < end_us) {
        flrc_stream2_header_t hdr;

        if(!hal.digitalRead(BENCH_PIN_DIO1)) {
            usleep((useconds_t)cfg.rx_poll_us);
            continue;
        }
        state = radio.readData(packet, cfg.payload_len);
        stats.last_state = state;
        if(state != RADIOLIB_ERR_NONE) {
            stats.errors++;
            (void)radio.finishReceive();
            hal.delayMicroseconds(250);
            (void)restart_continuous_rx(hal, radio, cfg, "STREAM2_RX_ERR");
            continue;
        }
        stats.packets++;
        stats.bytes += cfg.payload_len;
        if(flrc_stream2_parse_packet(packet, cfg.payload_len, &hdr) != 0) {
            stats.errors++;
            bad_chunks++;
            continue;
        }

        if(!started || hdr.session != current_session ||
           hdr.frame_id != current_frame) {
            size_t chunk_len = cfg.payload_len - FLRC_STREAM2_HDR_LEN;
            uint32_t expected_chunks =
                (hdr.frame_size + (uint32_t)chunk_len - 1U) /
                (uint32_t)chunk_len;

            if(hdr.frame_size > 65535U || hdr.chunk_count > 512U ||
               hdr.chunk_count != expected_chunks) {
                stats.errors++;
                bad_chunks++;
                continue;
            }
            if(started && received_chunks < current_chunks) {
                dropped_frames++;
            }
            started = 1;
            current_session = hdr.session;
            current_frame = hdr.frame_id;
            current_size = hdr.frame_size;
            current_crc = hdr.frame_crc;
            current_chunks = hdr.chunk_count;
            received_chunks = 0;
            frame.assign(current_size, 0);
            seen.assign(current_chunks, 0);
        }
        if(hdr.frame_size != current_size || hdr.frame_crc != current_crc ||
           hdr.chunk_count != current_chunks) {
            stats.errors++;
            bad_chunks++;
            continue;
        }
        if(seen[hdr.chunk_id]) {
            duplicate_chunks++;
            continue;
        }
        {
            size_t chunk_len = cfg.payload_len - FLRC_STREAM2_HDR_LEN;
            size_t offset = (size_t)hdr.chunk_id * chunk_len;
            if(offset + hdr.payload_len > frame.size()) {
                stats.errors++;
                bad_chunks++;
                continue;
            }
            memcpy(frame.data() + offset, packet + FLRC_STREAM2_HDR_LEN,
                   hdr.payload_len);
        }
        seen[hdr.chunk_id] = 1;
        received_chunks++;
        if(received_chunks == current_chunks) {
            uint32_t got_crc = crc32_update(0, frame.data(), frame.size());
            if(got_crc == current_crc) {
                char final_path[PATH_MAX];
                char tmp_path[PATH_MAX];
                FILE *fp;

                snprintf(tmp_path, sizeof(tmp_path), "%s/%010u.jpg.tmp",
                         cfg.out_dir, current_frame);
                snprintf(final_path, sizeof(final_path), "%s/%010u.jpg",
                         cfg.out_dir, current_frame);
                fp = fopen(tmp_path, "wb");
                if(fp) {
                    fwrite(frame.data(), 1, frame.size(), fp);
                    fclose(fp);
                    rename(tmp_path, final_path);
                    completed_frames++;
                    write_stream2_meta(cfg.out_dir, final_path,
                                       cfg.frame_width, cfg.frame_height,
                                       completed_frames, dropped_frames,
                                       bad_chunks, stats.packets, stats.bytes,
                                       start_us);
                    printf("STREAM2_RX_FRAME id=%u bytes=%zu chunks=%u completed=%llu dropped=%llu bad=%llu dup=%llu\n",
                           current_frame, frame.size(), current_chunks,
                           (unsigned long long)completed_frames,
                           (unsigned long long)dropped_frames,
                           (unsigned long long)bad_chunks,
                           (unsigned long long)duplicate_chunks);
                    fflush(stdout);
                } else {
                    stats.errors++;
                }
            } else {
                stats.errors++;
                bad_chunks++;
                printf("STREAM2_RX_FRAME_BAD_CRC id=%u got=0x%08x expected=0x%08x chunks=%u\n",
                       current_frame, got_crc, current_crc, current_chunks);
            }
            started = 0;
            current_session = 0;
            current_frame = 0;
            current_size = 0;
            current_crc = 0;
            current_chunks = 0;
            received_chunks = 0;
            frame.clear();
            seen.clear();
        }

        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            uint64_t elapsed_us = monotonic_us() - start_us;
            double mbps = elapsed_us ?
                ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;
            stats.last_log_us = monotonic_us();
            printf("STATS role=%s mbps=%.4f packets=%llu bytes=%llu errors=%llu frames=%llu dropped=%llu bad_chunks=%llu dup=%llu current=%u/%u elapsed_ms=%llu\n",
                   cfg.role, mbps, (unsigned long long)stats.packets,
                   (unsigned long long)stats.bytes,
                   (unsigned long long)stats.errors,
                   (unsigned long long)completed_frames,
                   (unsigned long long)dropped_frames,
                   (unsigned long long)bad_chunks,
                   (unsigned long long)duplicate_chunks,
                   received_chunks, current_chunks,
                   (unsigned long long)(elapsed_us / 1000ULL));
            fflush(stdout);
        }
    }
    (void)radio.standby();
    {
        uint64_t elapsed_us = monotonic_us() - start_us;
        double mbps = elapsed_us ?
            ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;
        printf("RESULT role=%s mbps=%.4f frames=%llu packets=%llu bytes=%llu errors=%llu dropped=%llu bad_chunks=%llu elapsed_ms=%llu\n",
               cfg.role, mbps, (unsigned long long)completed_frames,
               (unsigned long long)stats.packets,
               (unsigned long long)stats.bytes,
               (unsigned long long)stats.errors,
               (unsigned long long)dropped_frames,
               (unsigned long long)bad_chunks,
               (unsigned long long)(elapsed_us / 1000ULL));
    }
    return completed_frames > 0U ? 0 : 1;
}

static int run_stream_queue_tx(K230BenchHal &hal, LR2021 &radio,
                               bench_config_t &cfg)
{
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    bench_stats_t stats = {};
    unsigned sent = 0;
    unsigned failed = 0;

    if(!cfg.in_dir || !cfg.in_dir[0]) {
        fprintf(stderr, "stream-queue-tx requires --in-dir\n");
        return 2;
    }
    printf("QUEUE_TX_WAIT in_dir=%s duration=%u fast=%d\n",
           cfg.in_dir, cfg.duration_s, cfg.fast_frame);
    while(monotonic_us() < end_us) {
        char path[PATH_MAX];
        int rc;

        if(!find_next_queue_file(cfg.in_dir, path, sizeof(path),
                                 &stats.dropped_queue_files)) {
            usleep(20000);
            continue;
        }

        cfg.file_path = path;
        rc = run_file_tx(hal, radio, cfg);
        if(rc == 0) {
            sent++;
        } else {
            failed++;
        }
        unlink(path);
        printf("QUEUE_TX_FRAME path=%s rc=%d sent=%u failed=%u dropped=%llu\n",
               path, rc, sent, failed,
               (unsigned long long)stats.dropped_queue_files);
        fflush(stdout);
    }
    printf("RESULT role=%s sent=%u failed=%u dropped=%llu elapsed_ms=%llu\n",
           cfg.role, sent, failed,
           (unsigned long long)stats.dropped_queue_files,
           (unsigned long long)((monotonic_us() - start_us) / 1000ULL));
    return sent > 0U ? 0 : 1;
}

static int run_file_rx(K230BenchHal &hal, LR2021 &radio,
                       const bench_config_t &cfg)
{
    bench_stats_t stats = {};
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint8_t *seen = NULL;
    uint32_t nack_seqs[RADIOLIB_LR2021_MAX_PACKET_LENGTH / 4U];
    FILE *fp = NULL;
    char filename[128] = "flrc_video.bin";
    char part_path[320] = "";
    char final_path[320] = "";
    uint32_t session = 0;
    uint32_t file_size = 0;
    uint32_t file_crc = 0;
    uint32_t expected_chunks = 0;
    uint32_t received_chunks = 0;
    unsigned chunk_len = cfg.payload_len - FLRC_VIDEO_HDR_LEN;
    unsigned nack_cap = flrc_video_nack_capacity(cfg.payload_len);
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    int complete = 0;
    int crc_ok = 0;
    int started = 0;
    unsigned retry_round = 0;
    unsigned crc_log_count = 0;
    unsigned completed_files = 0;
    uint64_t completed_bytes = 0;
    uint32_t last_done_session = 0;
    uint32_t last_done_file_size = 0;
    uint32_t last_done_file_crc = 0;
    unsigned last_done_round = 0;
    uint64_t last_done_reply_us = 0;
    int16_t state;

    auto reset_rx_frame = [&]() {
        if(fp) {
            fclose(fp);
            fp = NULL;
        }
        if(part_path[0]) {
            unlink(part_path);
        }
        free(seen);
        seen = NULL;
        started = 0;
        complete = 0;
        crc_ok = 0;
        session = 0;
        file_size = 0;
        file_crc = 0;
        expected_chunks = 0;
        received_chunks = 0;
        retry_round = 0;
        crc_log_count = 0;
        part_path[0] = '\0';
        final_path[0] = '\0';
        filename[0] = '\0';
    };

    mkdir(cfg.out_dir, 0755);
    state = restart_continuous_rx(hal, radio, cfg, "FILE_RX");
    if(state != RADIOLIB_ERR_NONE) {
        printf("FILE_RX start state=%d %s\n", state, error_name(state));
        return 2;
    }
    printf("FILE_RX_WAIT out_dir=%s duration=%u chunk=%u\n", cfg.out_dir,
           cfg.duration_s, chunk_len);

    while(monotonic_us() < end_us && !complete) {
        if(!hal.digitalRead(BENCH_PIN_DIO1)) {
            usleep((useconds_t)cfg.rx_poll_us);
        } else {
            flrc_video_header_t hdr;

            state = radio.readData(packet, cfg.payload_len);
            stats.last_state = state;
            if(state != RADIOLIB_ERR_NONE) {
                stats.errors++;
                (void)radio.finishReceive();
                hal.delayMicroseconds(250);
                (void)restart_continuous_rx(hal, radio, cfg, "FILE_RX");
                continue;
            }
            stats.packets++;
            stats.bytes += cfg.payload_len;
            if(flrc_video_parse_packet(packet, cfg.payload_len, &hdr) != 0) {
                stats.errors++;
                continue;
            }

            if(hdr.type == FLRC_VIDEO_TYPE_START) {
                uint16_t name_len = hdr.payload_len;

                if(started && session == hdr.session) {
                    continue;
                }
                if(started && cfg.fast_frame) {
                    printf("FILE_RX_FAST_DROP_OLD session=0x%08x new=0x%08x chunks=%u/%u\n",
                           session, hdr.session, received_chunks,
                           expected_chunks);
                    reset_rx_frame();
                }
                if(hdr.seq != 0U || hdr.total_size == 0U ||
                   hdr.payload_len > chunk_len) {
                    stats.errors++;
                    printf("FILE_RX_BAD_START session=0x%08x payload_len=%u size=%u crc=0x%08x\n",
                           hdr.session, name_len, hdr.total_size, hdr.crc32);
                    continue;
                }
                if(fp) {
                    fclose(fp);
                    fp = NULL;
                }
                free(seen);
                seen = NULL;
                started = 1;
                session = hdr.session;
                file_size = hdr.total_size;
                file_crc = hdr.crc32;
                expected_chunks = (file_size + chunk_len - 1U) / chunk_len;
                received_chunks = 0;
                retry_round = 0;
                crc_log_count = 0;
                if(!flrc_video_sanitize_name_payload(
                       packet + FLRC_VIDEO_HDR_LEN, name_len, filename,
                       sizeof(filename), session)) {
                    printf("FILE_RX_START_NAME_REPAIRED session=0x%08x name=%s payload_len=%u\n",
                           session, filename, name_len);
                }
                if(cfg.fast_frame) {
                    ensure_jpeg_filename(filename, sizeof(filename));
                }
                snprintf(part_path, sizeof(part_path), "%s/%s.flrc.part",
                         cfg.out_dir, filename);
                snprintf(final_path, sizeof(final_path), "%s/%s",
                         cfg.out_dir, filename);
                seen = (uint8_t *)calloc(expected_chunks ? expected_chunks : 1U,
                                         1);
                fp = fopen(part_path, "wb+");
                if(!seen || !fp) {
                    printf("FILE_RX open failed file=%s seen=%p fp=%p errno=%s\n",
                           part_path, (void *)seen, (void *)fp, strerror(errno));
                    stats.errors++;
                    break;
                }
                printf("FILE_RX_START name=%s bytes=%u crc=0x%08x chunks=%u session=0x%08x\n",
                       filename, file_size, file_crc, expected_chunks, session);
            } else if(hdr.type == FLRC_VIDEO_TYPE_DATA) {
                uint32_t payload_crc;
                long offset;

                if(!started || hdr.session != session || !fp || !seen ||
                   hdr.seq >= expected_chunks) {
                    stats.errors++;
                    continue;
                }
                payload_crc = crc32_update(0, packet + FLRC_VIDEO_HDR_LEN,
                                           hdr.payload_len);
                if(payload_crc != hdr.crc32) {
                    stats.errors++;
                    if(crc_log_count < 20U || (crc_log_count % 200U) == 0U) {
                        printf("FILE_RX crc seq=%u got=0x%08x expected=0x%08x\n",
                               hdr.seq, payload_crc, hdr.crc32);
                    }
                    crc_log_count++;
                    continue;
                }
                offset = (long)hdr.seq * (long)chunk_len;
                if(fseek(fp, offset, SEEK_SET) != 0 ||
                   fwrite(packet + FLRC_VIDEO_HDR_LEN, 1, hdr.payload_len, fp) !=
                   hdr.payload_len) {
                    stats.errors++;
                    printf("FILE_RX write failed seq=%u errno=%s\n", hdr.seq,
                           strerror(errno));
                    continue;
                }
                if(!seen[hdr.seq]) {
                    seen[hdr.seq] = 1;
                    received_chunks++;
                }
            } else if(hdr.type == FLRC_VIDEO_TYPE_END) {
                if(!started && cfg.keep_listening &&
                   hdr.session == last_done_session &&
                   hdr.total_size == last_done_file_size &&
                   hdr.crc32 == last_done_file_crc) {
                    uint64_t now_us = monotonic_us();

                    if(cfg.fast_frame) {
                        printf("FILE_RX_FAST_DONE_REPEAT_IGNORED session=0x%08x round=%u\n",
                               hdr.session, last_done_round);
                        continue;
                    }
                    if(last_done_reply_us != 0ULL &&
                       now_us - last_done_reply_us <
                       (uint64_t)FLRC_VIDEO_DONE_REPEAT_MIN_MS * 1000ULL) {
                        printf("FILE_RX_DONE_REPEAT_IGNORED session=0x%08x round=%u\n",
                               hdr.session, last_done_round);
                    } else {
                        printf("FILE_RX_DONE_REPEAT session=0x%08x round=%u\n",
                               hdr.session, last_done_round);
                        hal.delay(FLRC_VIDEO_CTRL_REPLY_DELAY_MS);
                        (void)flrc_video_send_done(hal, radio, cfg, &stats,
                                                   packet, last_done_session,
                                                   last_done_round,
                                                   last_done_file_size,
                                                   last_done_file_crc);
                        last_done_reply_us = monotonic_us();
                    }
                    state = restart_continuous_rx(hal, radio, cfg,
                                                  "FILE_RX_REPEAT");
                    if(state != RADIOLIB_ERR_NONE) {
                        stats.errors++;
                        stats.last_state = state;
                        printf("FILE_RX repeat restart state=%d %s\n", state,
                               error_name(state));
                        break;
                    }
                } else if(started && hdr.session == session) {
                    uint32_t rx_crc_now = 0;
                    uint32_t rx_size_now = 0;
                    unsigned nack_count = 0;

                    printf("FILE_RX_END chunks=%u/%u crc=0x%08x\n",
                           received_chunks, expected_chunks, hdr.crc32);
                    retry_round++;
                    if(fp) {
                        fflush(fp);
                        fsync(fileno(fp));
                    }
                if(received_chunks == expected_chunks &&
                   file_crc32(part_path, &rx_crc_now, &rx_size_now) == 0 &&
                   rx_crc_now == file_crc && rx_size_now == file_size) {
                        crc_ok = 1;
                        complete = 1;
                        printf("FILE_RX_DONE round=%u crc=0x%08x\n",
                               retry_round, rx_crc_now);
                        hal.delay(cfg.fast_frame ?
                                  FLRC_VIDEO_FAST_CTRL_REPLY_DELAY_MS :
                                  FLRC_VIDEO_CTRL_REPLY_DELAY_MS);
                        (void)flrc_video_send_done(hal, radio, cfg, &stats,
                                                   packet, session,
                                                   retry_round, file_size,
                                                   file_crc);
                        last_done_reply_us = monotonic_us();
                        last_done_session = session;
                        last_done_file_size = file_size;
                        last_done_file_crc = file_crc;
                        last_done_round = retry_round;
                        if(cfg.keep_listening) {
                            if(fp) {
                                fclose(fp);
                                fp = NULL;
                            }
                            unlink(final_path);
                            if(rename(part_path, final_path) == 0) {
                                completed_files++;
                                completed_bytes += file_size;
                                printf("RESULT role=%s complete=1 crc_ok=1 file_bytes=%u rx_bytes=%u chunks=%u/%u packets=%llu errors=%llu crc=0x%08x rx_crc=0x%08x retries=%u file_index=%u elapsed_ms=%llu path=%s\n",
                                       cfg.role, file_size, rx_size_now,
                                       received_chunks, expected_chunks,
                                       (unsigned long long)stats.packets,
                                       (unsigned long long)stats.errors,
                                       file_crc, rx_crc_now, retry_round,
                                       completed_files,
                                       (unsigned long long)((monotonic_us() - start_us) / 1000ULL),
                                       final_path);
                            } else {
                                stats.errors++;
                                printf("FILE_RX rename failed %s -> %s: %s\n",
                                       part_path, final_path, strerror(errno));
                            }
                            fflush(stdout);
                            free(seen);
                            seen = NULL;
                            started = 0;
                            complete = 0;
                            crc_ok = 0;
                            session = 0;
                            file_size = 0;
                            file_crc = 0;
                            expected_chunks = 0;
                            received_chunks = 0;
                            part_path[0] = '\0';
                            final_path[0] = '\0';
                            filename[0] = '\0';
                            state = restart_continuous_rx(hal, radio, cfg,
                                                          "FILE_RX_NEXT");
                            if(state != RADIOLIB_ERR_NONE) {
                                stats.errors++;
                                stats.last_state = state;
                                printf("FILE_RX next restart state=%d %s\n", state,
                                       error_name(state));
                                break;
                            }
                        }
                    } else {
                        if(cfg.fast_frame) {
                            if(received_chunks == expected_chunks) {
                                printf("FILE_RX_FAST_CRC_MISMATCH got=0x%08x size=%u expected_crc=0x%08x size=%u\n",
                                       rx_crc_now, rx_size_now, file_crc,
                                       file_size);
                                reset_rx_frame();
                                continue;
                            }
                            if(retry_round <= FLRC_VIDEO_FAST_REPAIR_ROUNDS) {
                                nack_count = flrc_video_collect_missing(
                                    seen, expected_chunks, nack_seqs,
                                    nack_cap);
                                printf("FILE_RX_FAST_NACK round=%u count=%u first=%u chunks=%u/%u\n",
                                       retry_round, nack_count,
                                       nack_count ? nack_seqs[0] : 0,
                                       received_chunks, expected_chunks);
                                if(nack_count > 0U) {
                                    hal.delay(FLRC_VIDEO_FAST_CTRL_REPLY_DELAY_MS);
                                    (void)flrc_video_send_nack(
                                        hal, radio, cfg, &stats, packet,
                                        session, retry_round, file_size,
                                        file_crc, nack_seqs, nack_count);
                                }
                                state = restart_continuous_rx(hal, radio, cfg,
                                                              "FILE_RX_FAST_NACK");
                                if(state != RADIOLIB_ERR_NONE) {
                                    stats.errors++;
                                    stats.last_state = state;
                                    printf("FILE_RX fast restart state=%d %s\n",
                                           state, error_name(state));
                                    break;
                                }
                                continue;
                            }
                            printf("FILE_RX_FAST_DROP round=%u chunks=%u/%u crc=0x%08x\n",
                                   retry_round, received_chunks,
                                   expected_chunks, file_crc);
                            reset_rx_frame();
                            continue;
                        }
                        if(received_chunks == expected_chunks) {
                            printf("FILE_RX_FINAL_CRC_MISMATCH got=0x%08x size=%u expected_crc=0x%08x size=%u; requesting full repair\n",
                                   rx_crc_now, rx_size_now, file_crc,
                                   file_size);
                            memset(seen, 0, expected_chunks);
                            received_chunks = 0;
                        }
                        nack_count = flrc_video_collect_missing(seen,
                                                               expected_chunks,
                                                               nack_seqs,
                                                               nack_cap);
                        printf("FILE_RX_NACK round=%u count=%u first=%u chunks=%u/%u\n",
                               retry_round, nack_count,
                               nack_count ? nack_seqs[0] : 0,
                               received_chunks, expected_chunks);
                        if(nack_count > 0U) {
                            hal.delay(FLRC_VIDEO_CTRL_REPLY_DELAY_MS);
                            (void)flrc_video_send_nack(hal, radio, cfg,
                                                       &stats, packet,
                                                       session, retry_round,
                                                       file_size, file_crc,
                                                       nack_seqs, nack_count);
                        }
                        state = restart_continuous_rx(hal, radio, cfg,
                                                      "FILE_RX_NACK");
                        if(state != RADIOLIB_ERR_NONE) {
                            stats.errors++;
                            stats.last_state = state;
                            printf("FILE_RX restart state=%d %s\n", state,
                                   error_name(state));
                            break;
                        }
                    }
                }
            }
        }

        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            uint64_t elapsed_us = monotonic_us() - start_us;
            double mbps = elapsed_us ?
                ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;

            stats.last_log_us = monotonic_us();
            printf("STATS role=file-rx mbps=%.4f packets=%llu bytes=%llu errors=%llu received_chunks=%u expected_chunks=%u bytes_air=%llu elapsed_ms=%llu\n",
                   mbps, (unsigned long long)stats.packets,
                   (unsigned long long)stats.bytes,
                   (unsigned long long)stats.errors, received_chunks,
                   expected_chunks, (unsigned long long)stats.bytes,
                   (unsigned long long)(elapsed_us / 1000ULL));
            fflush(stdout);
        }
    }

    if(fp) {
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        fp = NULL;
    }
    (void)radio.standby();

    if(started && part_path[0]) {
        uint32_t rx_crc = 0;
        uint32_t rx_size = 0;

        if(received_chunks == expected_chunks &&
           file_crc32(part_path, &rx_crc, &rx_size) == 0 &&
           rx_crc == file_crc && rx_size == file_size) {
            unlink(final_path);
            if(rename(part_path, final_path) == 0) {
                crc_ok = 1;
            }
        }
        printf("RESULT role=%s complete=%d crc_ok=%d file_bytes=%u rx_bytes=%u chunks=%u/%u packets=%llu errors=%llu crc=0x%08x rx_crc=0x%08x retries=%u elapsed_ms=%llu path=%s\n",
               cfg.role, complete, crc_ok, file_size, rx_size, received_chunks,
               expected_chunks, (unsigned long long)stats.packets,
               (unsigned long long)stats.errors, file_crc, rx_crc,
               retry_round,
               (unsigned long long)((monotonic_us() - start_us) / 1000ULL),
               crc_ok ? final_path : part_path);
        free(seen);
        return crc_ok ? 0 : 1;
    }

    printf("RESULT role=%s complete=%d crc_ok=0 file_bytes=0 rx_bytes=0 chunks=0/0 packets=%llu errors=%llu files=%u completed_bytes=%llu elapsed_ms=%llu path=none\n",
           cfg.role, completed_files > 0U ? 1 : 0,
           (unsigned long long)stats.packets,
           (unsigned long long)stats.errors, completed_files,
           (unsigned long long)completed_bytes,
           (unsigned long long)((monotonic_us() - start_us) / 1000ULL));
    free(seen);
    return completed_files > 0U ? 0 : 1;
}

static int parse_uint(const char *text, unsigned *out);
static int parse_float_arg(const char *text, float *out);

#ifdef K230_FLRC_TILE_STREAM
static void flrc_tile_make_packet(uint8_t *packet, unsigned packet_len,
                                  uint32_t session, uint32_t frame_id,
                                  uint16_t tile_id, uint16_t tile_count,
                                  uint16_t x, uint16_t y, uint16_t w,
                                  uint16_t h, const uint8_t *payload,
                                  uint16_t payload_len)
{
    memset(packet, 0, packet_len);
    put_le32(packet + 0, FLRC_TILE_MAGIC);
    packet[4] = FLRC_TILE_VERSION;
    packet[5] = 1U;
    put_le16(packet + 6, payload_len);
    put_le32(packet + 8, session);
    put_le32(packet + 12, frame_id);
    put_le16(packet + 16, tile_id);
    put_le16(packet + 18, tile_count);
    put_le16(packet + 20, x);
    put_le16(packet + 22, y);
    put_le16(packet + 24, w);
    put_le16(packet + 26, h);
    put_le32(packet + 28, crc32_update(0, payload, payload_len));
    if(payload && payload_len > 0U &&
       FLRC_TILE_HDR_LEN + payload_len <= packet_len) {
        memcpy(packet + FLRC_TILE_HDR_LEN, payload, payload_len);
    }
}

static int flrc_tile_parse_packet(const uint8_t *packet, unsigned packet_len,
                                  flrc_tile_header_t *hdr)
{
    if(!packet || !hdr || packet_len < FLRC_TILE_HDR_LEN) {
        return -1;
    }
    if(get_le32(packet + 0) != FLRC_TILE_MAGIC ||
       packet[4] != FLRC_TILE_VERSION || packet[5] != 1U) {
        return -1;
    }
    hdr->payload_len = get_le16(packet + 6);
    hdr->session = get_le32(packet + 8);
    hdr->frame_id = get_le32(packet + 12);
    hdr->tile_id = get_le16(packet + 16);
    hdr->tile_count = get_le16(packet + 18);
    hdr->x = get_le16(packet + 20);
    hdr->y = get_le16(packet + 22);
    hdr->w = get_le16(packet + 24);
    hdr->h = get_le16(packet + 26);
    hdr->crc32 = get_le32(packet + 28);
    if(hdr->payload_len > packet_len - FLRC_TILE_HDR_LEN) {
        return -1;
    }
    return 0;
}

static int tile_parse_uint_arg(const char *text, unsigned *out)
{
    return parse_uint(text, out);
}

static int flrc_tile_load_frame(const char *path, uint8_t *buf,
                                size_t expected)
{
    FILE *fp = fopen(path, "rb");
    size_t got;

    if(!fp) {
        return -1;
    }
    got = fread(buf, 1, expected, fp);
    fclose(fp);
    return got == expected ? 0 : -1;
}

static void flrc_tile_store_tile(uint8_t *canvas, const uint8_t *payload,
                                 unsigned canvas_w, unsigned x, unsigned y,
                                 unsigned w, unsigned h)
{
    for(unsigned row = 0; row < h; row++) {
        memcpy(canvas + ((y + row) * canvas_w + x) * 2U,
               payload + row * w * 2U, w * 2U);
    }
}

static int flrc_tile_write_preview(const tile_config_t &cfg,
                                   const uint8_t *canvas)
{
    char tmp[PATH_MAX];
    FILE *fp;
    size_t bytes = (size_t)cfg.width * cfg.height * 2U;

    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.preview_path);
    fp = fopen(tmp, "wb");
    if(!fp) {
        return -1;
    }
    if(fwrite(canvas, 1, bytes, fp) != bytes) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    if(fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, cfg.preview_path);
}

static void flrc_tile_print_stats(const char *tag, const tile_config_t &cfg,
                                  const tile_stats_t &stats,
                                  uint64_t start_us)
{
    uint64_t elapsed_us = monotonic_us() - start_us;
    double mbps = elapsed_us ?
        ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;

    printf("%s role=%s mbps=%.4f packets=%llu bytes=%llu errors=%llu frames=%llu tiles=%llu bad_crc=%llu dup=%llu dropped=%llu last=%d %ux%u tile=%ux%u repeat=%u freq=%.1f br=%u elapsed_ms=%llu\n",
           tag, cfg.role, mbps,
           (unsigned long long)stats.packets,
           (unsigned long long)stats.bytes,
           (unsigned long long)stats.errors,
           (unsigned long long)stats.frames,
           (unsigned long long)stats.tiles,
           (unsigned long long)stats.bad_crc,
           (unsigned long long)stats.duplicate_tiles,
           (unsigned long long)stats.dropped_queue_frames,
           stats.last_state, cfg.width, cfg.height, cfg.tile_w, cfg.tile_h,
           cfg.repeat, cfg.freq_mhz, cfg.bitrate_kbps,
           (unsigned long long)(elapsed_us / 1000ULL));
    fflush(stdout);
}

static int flrc_tile_tx_packet(K230BenchHal &hal, LR2021 &radio,
                               const tile_config_t &cfg, tile_stats_t *stats,
                               uint8_t *packet)
{
    int16_t state = fast_transmit(hal, radio, packet, cfg.payload_len,
                                  cfg.bitrate_kbps);
    stats->last_state = state;
    if(state == RADIOLIB_ERR_NONE) {
        stats->packets++;
        stats->bytes += cfg.payload_len;
        return 0;
    }
    stats->errors++;
    if(stats->errors < 10ULL || (stats->errors % 100ULL) == 0ULL) {
        printf("TILE_TX state=%d %s\n", state, error_name(state));
    }
    return -1;
}

static int run_tile_tx_frame(K230BenchHal &hal, LR2021 &radio,
                             const tile_config_t &cfg, tile_stats_t *stats,
                             const uint8_t *frame, uint32_t session,
                             uint32_t frame_id, uint8_t *packet,
                             uint8_t *tile)
{
    unsigned cols = (cfg.width + cfg.tile_w - 1U) / cfg.tile_w;
    unsigned rows = (cfg.height + cfg.tile_h - 1U) / cfg.tile_h;
    unsigned tile_count = cols * rows;
    unsigned tile_id = 0;

    for(unsigned ty = 0; ty < rows; ty++) {
        for(unsigned tx = 0; tx < cols; tx++) {
            unsigned x = tx * cfg.tile_w;
            unsigned y = ty * cfg.tile_h;
            unsigned w = std::min(cfg.tile_w, cfg.width - x);
            unsigned h = std::min(cfg.tile_h, cfg.height - y);
            unsigned payload_len = w * h * 2U;

            for(unsigned row = 0; row < h; row++) {
                memcpy(tile + row * w * 2U,
                       frame + ((y + row) * cfg.width + x) * 2U,
                       w * 2U);
            }
            flrc_tile_make_packet(packet, cfg.payload_len, session, frame_id,
                                  (uint16_t)tile_id, (uint16_t)tile_count,
                                  (uint16_t)x, (uint16_t)y, (uint16_t)w,
                                  (uint16_t)h, tile, (uint16_t)payload_len);
            for(unsigned r = 0; r < cfg.repeat; r++) {
                (void)flrc_tile_tx_packet(hal, radio, cfg, stats, packet);
                if(cfg.tile_gap_us > 0U) {
                    hal.delayMicroseconds(cfg.tile_gap_us);
                }
            }
            stats->tiles++;
            tile_id++;
        }
    }
    stats->frames++;
    if(cfg.frame_gap_us > 0U) {
        hal.delayMicroseconds(cfg.frame_gap_us);
    }
    return 0;
}

static int find_next_raw_queue_file(const char *dir, char *path,
                                    size_t path_len, uint64_t *dropped)
{
    DIR *dp;
    struct dirent *de;
    char best[NAME_MAX + 1] = "";
    std::vector<std::string> stale;

    dp = opendir(dir);
    if(!dp) {
        return 0;
    }
    while((de = readdir(dp)) != NULL) {
        size_t len = strlen(de->d_name);

        if(len < 5U || strcmp(de->d_name + len - 4U, ".raw") != 0) {
            continue;
        }
        if(best[0] != '\0' && strcmp(de->d_name, best) < 0) {
            stale.push_back(de->d_name);
            continue;
        }
        if(best[0] != '\0') {
            stale.push_back(best);
        }
        if(best[0] == '\0' || strcmp(de->d_name, best) > 0) {
            snprintf(best, sizeof(best), "%s", de->d_name);
        }
    }
    closedir(dp);
    if(best[0] == '\0') {
        return 0;
    }
    for(const std::string &name : stale) {
        char stale_path[PATH_MAX];

        snprintf(stale_path, sizeof(stale_path), "%s/%s", dir, name.c_str());
        if(unlink(stale_path) == 0 && dropped) {
            (*dropped)++;
        }
    }
    snprintf(path, path_len, "%s/%s", dir, best);
    return 1;
}

static int run_tile_tx(K230BenchHal &hal, LR2021 &radio,
                       const tile_config_t &cfg)
{
    tile_stats_t stats = {};
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    size_t frame_bytes = (size_t)cfg.width * cfg.height * 2U;
    size_t max_tile_bytes = (size_t)cfg.tile_w * cfg.tile_h * 2U;
    std::vector<uint8_t> frame(frame_bytes);
    std::vector<uint8_t> tile(max_tile_bytes);
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint32_t session = (uint32_t)monotonic_us();
    uint32_t frame_id = 0;

    printf("TILE_TX_WAIT in_dir=%s file=%s duration=%u\n",
           cfg.in_dir ? cfg.in_dir : "", cfg.frame_path ? cfg.frame_path : "",
           cfg.duration_s);
    while(monotonic_us() < end_us) {
        char path[PATH_MAX];

        if(cfg.in_dir && cfg.in_dir[0]) {
            if(!find_next_raw_queue_file(cfg.in_dir, path, sizeof(path),
                                         &stats.dropped_queue_frames)) {
                usleep(15000);
                continue;
            }
        } else if(cfg.frame_path && cfg.frame_path[0]) {
            snprintf(path, sizeof(path), "%s", cfg.frame_path);
        } else {
            fprintf(stderr, "tile tx requires --in-dir or --frame\n");
            return 2;
        }

        if(flrc_tile_load_frame(path, frame.data(), frame_bytes) != 0) {
            printf("TILE_TX_LOAD_FAIL path=%s\n", path);
            if(cfg.in_dir && cfg.in_dir[0]) {
                unlink(path);
            } else {
                usleep(50000);
            }
            continue;
        }
        run_tile_tx_frame(hal, radio, cfg, &stats, frame.data(), session,
                          frame_id++, packet, tile.data());
        if(cfg.in_dir && cfg.in_dir[0]) {
            unlink(path);
        }
        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            stats.last_log_us = monotonic_us();
            flrc_tile_print_stats("STATS", cfg, stats, start_us);
        }
    }
    (void)radio.standby();
    flrc_tile_print_stats("RESULT", cfg, stats, start_us);
    return stats.frames > 0ULL ? 0 : 1;
}

static int run_tile_rx(K230BenchHal &hal, LR2021 &radio,
                       const tile_config_t &cfg)
{
    tile_stats_t stats = {};
    uint64_t start_us = monotonic_us();
    uint64_t end_us = start_us + (uint64_t)cfg.duration_s * 1000000ULL;
    size_t frame_bytes = (size_t)cfg.width * cfg.height * 2U;
    std::vector<uint8_t> canvas(frame_bytes);
    std::vector<uint8_t> seen;
    uint8_t packet[RADIOLIB_LR2021_MAX_PACKET_LENGTH];
    uint32_t current_session = 0;
    uint32_t current_frame = 0;
    uint16_t current_tile_count = 0;
    uint16_t current_seen = 0;
    int have_frame = 0;
    int16_t state;

    std::fill(canvas.begin(), canvas.end(), 0);
    (void)flrc_tile_write_preview(cfg, canvas.data());
    state = start_continuous_rx(radio, cfg.payload_len);
    if(state != RADIOLIB_ERR_NONE) {
        printf("TILE_RX start state=%d %s\n", state, error_name(state));
        return 2;
    }
    printf("TILE_RX_WAIT preview=%s duration=%u %ux%u\n", cfg.preview_path,
           cfg.duration_s, cfg.width, cfg.height);
    while(monotonic_us() < end_us) {
        flrc_tile_header_t hdr;

        if(!hal.digitalRead(BENCH_PIN_DIO1)) {
            usleep((useconds_t)cfg.rx_poll_us);
        } else {
            state = radio.readData(packet, cfg.payload_len);
            stats.last_state = state;
            if(state != RADIOLIB_ERR_NONE) {
                stats.errors++;
                (void)radio.finishReceive();
                hal.delayMicroseconds(180);
                state = start_continuous_rx(radio, cfg.payload_len);
                if(state != RADIOLIB_ERR_NONE) {
                    stats.last_state = state;
                    printf("TILE_RX restart state=%d %s\n", state,
                           error_name(state));
                    break;
                }
                continue;
            }
            stats.packets++;
            stats.bytes += cfg.payload_len;
            if(flrc_tile_parse_packet(packet, cfg.payload_len, &hdr) != 0 ||
               hdr.w == 0U || hdr.h == 0U ||
               hdr.x + hdr.w > cfg.width || hdr.y + hdr.h > cfg.height ||
               hdr.payload_len != hdr.w * hdr.h * 2U ||
               hdr.payload_len + FLRC_TILE_HDR_LEN > cfg.payload_len) {
                stats.errors++;
                continue;
            }
            if(crc32_update(0, packet + FLRC_TILE_HDR_LEN,
                            hdr.payload_len) != hdr.crc32) {
                stats.bad_crc++;
                stats.errors++;
                continue;
            }

            if(!have_frame || hdr.session != current_session ||
               hdr.frame_id != current_frame) {
                current_session = hdr.session;
                current_frame = hdr.frame_id;
                current_tile_count = hdr.tile_count;
                current_seen = 0;
                seen.assign(current_tile_count ? current_tile_count : 1U, 0);
                have_frame = 1;
                stats.frames++;
            }
            if(hdr.tile_id < seen.size()) {
                if(seen[hdr.tile_id]) {
                    stats.duplicate_tiles++;
                } else {
                    seen[hdr.tile_id] = 1;
                    current_seen++;
                }
            }
            flrc_tile_store_tile(canvas.data(), packet + FLRC_TILE_HDR_LEN,
                                 cfg.width, hdr.x, hdr.y, hdr.w, hdr.h);
            stats.tiles++;
            if(current_seen == current_tile_count ||
               (stats.tiles % 4ULL) == 0ULL) {
                (void)flrc_tile_write_preview(cfg, canvas.data());
            }
        }

        if(monotonic_us() - stats.last_log_us >= 1000000ULL) {
            uint64_t elapsed_us = monotonic_us() - start_us;
            double mbps = elapsed_us ?
                ((double)stats.bytes * 8.0) / (double)elapsed_us : 0.0;

            stats.last_log_us = monotonic_us();
            printf("STATS role=%s mbps=%.4f packets=%llu bytes=%llu errors=%llu frames=%llu tiles=%llu current=%u/%u bad_crc=%llu dup=%llu dropped=%llu last=%d %ux%u tile=%ux%u repeat=%u freq=%.1f br=%u elapsed_ms=%llu\n",
                   cfg.role, mbps,
                   (unsigned long long)stats.packets,
                   (unsigned long long)stats.bytes,
                   (unsigned long long)stats.errors,
                   (unsigned long long)stats.frames,
                   (unsigned long long)stats.tiles, current_seen,
                   current_tile_count,
                   (unsigned long long)stats.bad_crc,
                   (unsigned long long)stats.duplicate_tiles,
                   (unsigned long long)stats.dropped_queue_frames,
                   stats.last_state, cfg.width, cfg.height, cfg.tile_w,
                   cfg.tile_h, cfg.repeat, cfg.freq_mhz, cfg.bitrate_kbps,
                   (unsigned long long)(elapsed_us / 1000ULL));
            fflush(stdout);
        }
    }
    (void)flrc_tile_write_preview(cfg, canvas.data());
    (void)radio.standby();
    flrc_tile_print_stats("RESULT", cfg, stats, start_us);
    return stats.tiles > 0ULL ? 0 : 1;
}

static void tile_usage(const char *prog)
{
    printf("Usage: %s --role tile-tx|tile-rx [options]\n", prog);
    printf("Options:\n");
    printf("  --in-dir DIR        TX raw RGB565 frame queue directory.\n");
    printf("  --frame PATH        TX one raw RGB565 frame repeatedly.\n");
    printf("  --preview PATH      RX RGB565 preview path. Default %s\n",
           FLRC_TILE_PREVIEW_PATH_DEFAULT);
    printf("  --width N           Default %u\n", FLRC_TILE_CANVAS_W_DEFAULT);
    printf("  --height N          Default %u\n", FLRC_TILE_CANVAS_H_DEFAULT);
    printf("  --tile-w N          Default %u\n", FLRC_TILE_WIDTH_DEFAULT);
    printf("  --tile-h N          Default %u\n", FLRC_TILE_HEIGHT_DEFAULT);
    printf("  --repeat N          Per-tile repeat. Default %u\n",
           FLRC_TILE_REPEAT_DEFAULT);
    printf("  --tile-gap-us N     Default %u\n", FLRC_TILE_GAP_US_DEFAULT);
    printf("  --frame-gap-us N    Default %u\n", FLRC_TILE_FRAME_GAP_US_DEFAULT);
    printf("  --freq MHz          Default %.1f\n", BENCH_FREQ_DEFAULT);
    printf("  --br KBPS           Default %u\n", BENCH_BR_DEFAULT);
    printf("  --duration SEC      Default %u\n", BENCH_DURATION_DEFAULT);
    printf("  --len BYTES         Default %u\n", BENCH_PAYLOAD_LEN_DEFAULT);
}

static int tile_main(int argc, char **argv)
{
    tile_config_t cfg = {
        .role = "",
        .frame_path = "",
        .in_dir = "",
        .preview_path = FLRC_TILE_PREVIEW_PATH_DEFAULT,
        .freq_mhz = BENCH_FREQ_DEFAULT,
        .bitrate_kbps = BENCH_BR_DEFAULT,
        .duration_s = BENCH_DURATION_DEFAULT,
        .payload_len = BENCH_PAYLOAD_LEN_DEFAULT,
        .spi_hz = BENCH_SPI_SPEED_DEFAULT,
        .power_dbm = BENCH_POWER_DEFAULT,
        .rx_poll_us = BENCH_RX_POLL_US_DEFAULT,
        .width = FLRC_TILE_CANVAS_W_DEFAULT,
        .height = FLRC_TILE_CANVAS_H_DEFAULT,
        .tile_w = FLRC_TILE_WIDTH_DEFAULT,
        .tile_h = FLRC_TILE_HEIGHT_DEFAULT,
        .repeat = FLRC_TILE_REPEAT_DEFAULT,
        .tile_gap_us = FLRC_TILE_GAP_US_DEFAULT,
        .frame_gap_us = FLRC_TILE_FRAME_GAP_US_DEFAULT,
    };

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            cfg.role = argv[++i];
        } else if(strcmp(argv[i], "--in-dir") == 0 && i + 1 < argc) {
            cfg.in_dir = argv[++i];
        } else if(strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
            cfg.frame_path = argv[++i];
        } else if(strcmp(argv[i], "--preview") == 0 && i + 1 < argc) {
            cfg.preview_path = argv[++i];
        } else if(strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            if(parse_float_arg(argv[++i], &cfg.freq_mhz) != 0) {
                fprintf(stderr, "invalid --freq\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--br") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.bitrate_kbps) != 0) {
                fprintf(stderr, "invalid --br\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.duration_s) != 0) {
                fprintf(stderr, "invalid --duration\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--len") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.payload_len) != 0) {
                fprintf(stderr, "invalid --len\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--spi-hz") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.spi_hz) != 0) {
                fprintf(stderr, "invalid --spi-hz\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--power") == 0 && i + 1 < argc) {
            cfg.power_dbm = atoi(argv[++i]);
        } else if(strcmp(argv[i], "--rx-poll-us") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.rx_poll_us) != 0) {
                fprintf(stderr, "invalid --rx-poll-us\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.width) != 0) {
                fprintf(stderr, "invalid --width\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.height) != 0) {
                fprintf(stderr, "invalid --height\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--tile-w") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.tile_w) != 0) {
                fprintf(stderr, "invalid --tile-w\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--tile-h") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.tile_h) != 0) {
                fprintf(stderr, "invalid --tile-h\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.repeat) != 0) {
                fprintf(stderr, "invalid --repeat\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--tile-gap-us") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.tile_gap_us) != 0) {
                fprintf(stderr, "invalid --tile-gap-us\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--frame-gap-us") == 0 && i + 1 < argc) {
            if(tile_parse_uint_arg(argv[++i], &cfg.frame_gap_us) != 0) {
                fprintf(stderr, "invalid --frame-gap-us\n");
                return 2;
            }
        } else if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            tile_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            tile_usage(argv[0]);
            return 2;
        }
    }

    if(strcmp(cfg.role, "tile-tx") != 0 &&
       strcmp(cfg.role, "tile-rx") != 0) {
        fprintf(stderr, "--role must be tile-tx or tile-rx\n");
        tile_usage(argv[0]);
        return 2;
    }
    if(cfg.payload_len <= FLRC_TILE_HDR_LEN ||
       cfg.payload_len > RADIOLIB_LR2021_MAX_PACKET_LENGTH) {
        fprintf(stderr, "--len must be %u..%u\n", FLRC_TILE_HDR_LEN + 1U,
                RADIOLIB_LR2021_MAX_PACKET_LENGTH);
        return 2;
    }
    if(cfg.width == 0U || cfg.height == 0U ||
       cfg.tile_w == 0U || cfg.tile_h == 0U ||
       cfg.repeat == 0U || cfg.duration_s == 0U) {
        fprintf(stderr, "invalid zero size/repeat/duration\n");
        return 2;
    }
    if((size_t)cfg.tile_w * cfg.tile_h * 2U >
       (size_t)(cfg.payload_len - FLRC_TILE_HDR_LEN)) {
        fprintf(stderr, "tile payload too large: %ux%u needs %u bytes, max %u\n",
                cfg.tile_w, cfg.tile_h, cfg.tile_w * cfg.tile_h * 2U,
                cfg.payload_len - FLRC_TILE_HDR_LEN);
        return 2;
    }

    K230BenchHal hal(BENCH_SPI_DEV, cfg.spi_hz);
    hal.pinMode(BENCH_PIN_POWER, K230_HAL_GPIO_OUTPUT);
    hal.digitalWrite(BENCH_PIN_POWER, K230_HAL_GPIO_HIGH);
    hal.delay(30);
    hal.spiBegin();
    if(!hal.spi_ready()) {
        fprintf(stderr, "SPI not ready: %s\n", hal.last_error());
        return 1;
    }

    {
        bench_config_t radio_cfg = {
            .role = cfg.role,
            .file_path = "",
            .in_dir = "",
            .out_dir = "",
            .freq_mhz = cfg.freq_mhz,
            .bitrate_kbps = cfg.bitrate_kbps,
            .duration_s = cfg.duration_s,
            .payload_len = cfg.payload_len,
            .spi_hz = cfg.spi_hz,
            .power_dbm = cfg.power_dbm,
            .rx_poll_us = cfg.rx_poll_us,
            .max_retries = 1,
            .ack_wait_ms = 100,
            .keep_listening = 0,
            .fast_frame = 1,
        };
        Module module(&hal, BENCH_PIN_CS, BENCH_PIN_DIO1, BENCH_PIN_RST,
                      BENCH_PIN_BUSY);
        LR2021 radio(&module);
        int16_t state = begin_flrc(radio, radio_cfg);
        if(state != RADIOLIB_ERR_NONE) {
            fprintf(stderr, "FLRC init failed: %d %s\n", state,
                    error_name(state));
            return 1;
        }
        printf("START role=%s freq=%.1f br=%u len=%u spi=%u power=%d duration=%u rx_poll=%u %ux%u tile=%ux%u repeat=%u\n",
               cfg.role, cfg.freq_mhz, cfg.bitrate_kbps, cfg.payload_len,
               cfg.spi_hz, cfg.power_dbm, cfg.duration_s, cfg.rx_poll_us,
               cfg.width, cfg.height, cfg.tile_w, cfg.tile_h, cfg.repeat);
        fflush(stdout);
        if(strcmp(cfg.role, "tile-tx") == 0) {
            return run_tile_tx(hal, radio, cfg);
        }
        return run_tile_rx(hal, radio, cfg);
    }
}
#endif

static void usage(const char *prog)
{
    printf("Usage: %s --role tx|rx|file-tx|file-rx|stream-tx|stream-rx|stream-queue-tx [options]\n", prog);
    printf("Options:\n");
    printf("  --freq MHz          Default %.1f\n", BENCH_FREQ_DEFAULT);
    printf("  --br KBPS           650, 1300, or 2600. Default %u\n",
           BENCH_BR_DEFAULT);
    printf("  --duration SEC      Default %u\n", BENCH_DURATION_DEFAULT);
    printf("  --len BYTES         1..255, default %u\n",
           BENCH_PAYLOAD_LEN_DEFAULT);
    printf("  --spi-hz HZ         Default %u\n", BENCH_SPI_SPEED_DEFAULT);
    printf("  --power DBM         LR2021 LILYGO HF safe range <= 4, default %d\n",
           BENCH_POWER_DEFAULT);
    printf("  --rx-poll-us USEC   RX DIO poll interval, default %u\n",
           BENCH_RX_POLL_US_DEFAULT);
    printf("  --in-dir DIR        Input queue directory for stream-queue-tx.\n");
    printf("  --file PATH         Input file for file/stream TX. Stream default: %s\n",
           FLRC_VIDEO_DEFAULT_FILE);
    printf("  --out-dir DIR       Output directory for file-rx. Default %s\n",
           FLRC_VIDEO_DEFAULT_OUT_DIR);
    printf("  --retries N         ACK/NACK repair rounds. Default %u\n",
           FLRC_VIDEO_MAX_RETRIES_DEFAULT);
    printf("  --ack-wait-ms MS    TX wait time for RX DONE/NACK. Default %u\n",
           FLRC_VIDEO_ACK_WAIT_MS_DEFAULT);
    printf("  --keep-listening    RX keeps accepting more files until duration expires.\n");
    printf("  --fast-frame        Low-latency JPEG preview mode.\n");
    printf("  --frame-width N     Stream v2 JPEG preview width. Default %u\n",
           FLRC_STREAM2_JPEG_WIDTH_DEFAULT);
    printf("  --frame-height N    Stream v2 JPEG preview height. Default %u\n",
           FLRC_STREAM2_JPEG_HEIGHT_DEFAULT);
    printf("  --frame-gap-us N    Stream v2 inter-packet gap. Default %u\n",
           FLRC_VIDEO_FAST_FRAME_GAP_US);
}

static int parse_uint(const char *text, unsigned *out)
{
    char *endp = nullptr;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &endp, 0);
    if(errno || endp == text || *endp != '\0' || value > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (unsigned)value;
    return 0;
}

static int parse_float_arg(const char *text, float *out)
{
    char *endp = nullptr;
    float value;

    errno = 0;
    value = strtof(text, &endp);
    if(errno || endp == text || *endp != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

#ifdef K230_FLRC_TILE_STREAM
int main(int argc, char **argv)
{
    return tile_main(argc, argv);
}
#else
int main(int argc, char **argv)
{
    bench_config_t cfg = {
        .role = "",
        .file_path = "",
        .in_dir = "",
        .out_dir = FLRC_VIDEO_DEFAULT_OUT_DIR,
        .freq_mhz = BENCH_FREQ_DEFAULT,
        .bitrate_kbps = BENCH_BR_DEFAULT,
        .duration_s = BENCH_DURATION_DEFAULT,
        .payload_len = BENCH_PAYLOAD_LEN_DEFAULT,
        .spi_hz = BENCH_SPI_SPEED_DEFAULT,
        .power_dbm = BENCH_POWER_DEFAULT,
        .rx_poll_us = BENCH_RX_POLL_US_DEFAULT,
        .max_retries = FLRC_VIDEO_MAX_RETRIES_DEFAULT,
        .ack_wait_ms = FLRC_VIDEO_ACK_WAIT_MS_DEFAULT,
        .keep_listening = 0,
        .fast_frame = 0,
        .frame_width = FLRC_STREAM2_JPEG_WIDTH_DEFAULT,
        .frame_height = FLRC_STREAM2_JPEG_HEIGHT_DEFAULT,
        .frame_gap_us = FLRC_VIDEO_FAST_FRAME_GAP_US,
    };

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            cfg.role = argv[++i];
        } else if(strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            if(parse_float_arg(argv[++i], &cfg.freq_mhz) != 0) {
                fprintf(stderr, "invalid --freq\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--br") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.bitrate_kbps) != 0) {
                fprintf(stderr, "invalid --br\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.duration_s) != 0) {
                fprintf(stderr, "invalid --duration\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--len") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.payload_len) != 0) {
                fprintf(stderr, "invalid --len\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--spi-hz") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.spi_hz) != 0) {
                fprintf(stderr, "invalid --spi-hz\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--power") == 0 && i + 1 < argc) {
            cfg.power_dbm = atoi(argv[++i]);
        } else if(strcmp(argv[i], "--rx-poll-us") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.rx_poll_us) != 0) {
                fprintf(stderr, "invalid --rx-poll-us\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--retries") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.max_retries) != 0) {
                fprintf(stderr, "invalid --retries\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--ack-wait-ms") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.ack_wait_ms) != 0) {
                fprintf(stderr, "invalid --ack-wait-ms\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            cfg.file_path = argv[++i];
        } else if(strcmp(argv[i], "--in-dir") == 0 && i + 1 < argc) {
            cfg.in_dir = argv[++i];
        } else if(strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            cfg.out_dir = argv[++i];
        } else if(strcmp(argv[i], "--keep-listening") == 0) {
            cfg.keep_listening = 1;
        } else if(strcmp(argv[i], "--fast-frame") == 0) {
            cfg.fast_frame = 1;
        } else if(strcmp(argv[i], "--frame-width") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.frame_width) != 0) {
                fprintf(stderr, "invalid --frame-width\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--frame-height") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.frame_height) != 0) {
                fprintf(stderr, "invalid --frame-height\n");
                return 2;
            }
        } else if(strcmp(argv[i], "--frame-gap-us") == 0 && i + 1 < argc) {
            if(parse_uint(argv[++i], &cfg.frame_gap_us) != 0) {
                fprintf(stderr, "invalid --frame-gap-us\n");
                return 2;
            }
        } else if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if(strcmp(cfg.role, "tx") != 0 && strcmp(cfg.role, "rx") != 0 &&
       strcmp(cfg.role, "file-tx") != 0 && strcmp(cfg.role, "file-rx") != 0 &&
       strcmp(cfg.role, "stream-tx") != 0 && strcmp(cfg.role, "stream-rx") != 0 &&
       strcmp(cfg.role, "stream-queue-tx") != 0 &&
       strcmp(cfg.role, "frame-stream-tx") != 0 &&
       strcmp(cfg.role, "frame-stream-rx") != 0) {
        fprintf(stderr, "--role must be tx, rx, file-tx, file-rx, stream-tx, stream-rx, stream-queue-tx, frame-stream-tx, or frame-stream-rx\n");
        usage(argv[0]);
        return 2;
    }
    if(strcmp(cfg.role, "stream-tx") == 0 && (!cfg.file_path || !cfg.file_path[0])) {
        cfg.file_path = FLRC_VIDEO_DEFAULT_FILE;
    }
    if(cfg.payload_len == 0 || cfg.payload_len > RADIOLIB_LR2021_MAX_PACKET_LENGTH) {
        fprintf(stderr, "--len must be 1..255\n");
        return 2;
    }
    if(cfg.duration_s == 0) {
        fprintf(stderr, "--duration must be positive\n");
        return 2;
    }
    if((strcmp(cfg.role, "file-tx") == 0 ||
        strcmp(cfg.role, "file-rx") == 0 ||
        strcmp(cfg.role, "stream-tx") == 0 ||
        strcmp(cfg.role, "stream-rx") == 0 ||
        strcmp(cfg.role, "stream-queue-tx") == 0) &&
       cfg.payload_len <= FLRC_VIDEO_HDR_LEN) {
        fprintf(stderr, "--len must be greater than %u for file transfer\n",
                FLRC_VIDEO_HDR_LEN);
        return 2;
    }
    if((strcmp(cfg.role, "frame-stream-tx") == 0 ||
        strcmp(cfg.role, "frame-stream-rx") == 0) &&
       cfg.payload_len <= FLRC_STREAM2_HDR_LEN) {
        fprintf(stderr, "--len must be greater than %u for frame stream\n",
                FLRC_STREAM2_HDR_LEN);
        return 2;
    }
    if(cfg.max_retries == 0) {
        fprintf(stderr, "--retries must be positive for reliable transfer\n");
        return 2;
    }
    if(cfg.ack_wait_ms < 100U) {
        fprintf(stderr, "--ack-wait-ms must be at least 100\n");
        return 2;
    }

    K230BenchHal hal(BENCH_SPI_DEV, cfg.spi_hz);
    hal.pinMode(BENCH_PIN_POWER, K230_HAL_GPIO_OUTPUT);
    hal.digitalWrite(BENCH_PIN_POWER, K230_HAL_GPIO_HIGH);
    hal.delay(30);
    hal.spiBegin();
    if(!hal.spi_ready()) {
        fprintf(stderr, "SPI not ready: %s\n", hal.last_error());
        return 1;
    }

    Module module(&hal, BENCH_PIN_CS, BENCH_PIN_DIO1, BENCH_PIN_RST,
                  BENCH_PIN_BUSY);
    LR2021 radio(&module);
    int16_t state = begin_flrc(radio, cfg);
    if(state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "FLRC init failed: %d %s\n", state,
                error_name(state));
        return 1;
    }

    printf("START role=%s freq=%.1f br=%u len=%u spi=%u power=%d duration=%u rx_poll=%u retries=%u ack_wait_ms=%u fast=%d\n",
           cfg.role, cfg.freq_mhz, cfg.bitrate_kbps, cfg.payload_len,
           cfg.spi_hz, cfg.power_dbm, cfg.duration_s, cfg.rx_poll_us,
           cfg.max_retries, cfg.ack_wait_ms, cfg.fast_frame);
    fflush(stdout);

    if(strcmp(cfg.role, "tx") == 0) {
        return run_tx(hal, radio, cfg);
    }
    if(strcmp(cfg.role, "rx") == 0) {
        return run_rx(hal, radio, cfg);
    }
    if(strcmp(cfg.role, "file-tx") == 0 || strcmp(cfg.role, "stream-tx") == 0) {
        return run_file_tx(hal, radio, cfg);
    }
    if(strcmp(cfg.role, "stream-queue-tx") == 0) {
        return run_stream_queue_tx(hal, radio, cfg);
    }
    if(strcmp(cfg.role, "frame-stream-tx") == 0) {
        return run_frame_stream_tx(hal, radio, cfg);
    }
    if(strcmp(cfg.role, "frame-stream-rx") == 0) {
        return run_frame_stream_rx(hal, radio, cfg);
    }
    return run_file_rx(hal, radio, cfg);
}
#endif
