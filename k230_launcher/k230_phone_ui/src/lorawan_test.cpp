#define main k230_flrc_bench_hidden_main
#include "lora_flrc_bench.cpp"
#undef main

#include "modules/SX126x/SX1262.h"
#include "protocols/LoRaWAN/LoRaWAN.h"

#include <ctype.h>
#include <strings.h>

#define LORAWAN_TEST_CONFIG_PATH "/root/lorawan/otaa.conf"
#define LORAWAN_TEST_SESSION_PATH "/root/lorawan/session.bin"
#define LORAWAN_TEST_NONCES_PATH "/root/lorawan/nonces.bin"

typedef enum {
    LORAWAN_TEST_CHIP_NONE = 0,
    LORAWAN_TEST_CHIP_SX1262,
    LORAWAN_TEST_CHIP_LR2021,
} lorawan_test_chip_t;

typedef struct {
    const char *name;
    const LoRaWANBand_t *band;
    float freq_mhz;
} lorawan_test_region_t;

typedef struct {
    const lorawan_test_region_t *region;
    uint8_t sub_band;
    uint64_t join_eui;
    uint64_t dev_eui;
    uint8_t app_key[16];
    uint8_t nwk_key[16];
    int has_join_eui;
    int has_dev_eui;
    int has_app_key;
    int has_nwk_key;
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
} lorawan_test_config_t;

static const lorawan_test_region_t lorawan_test_regions[] = {
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

static const uint32_t lorawan_lr2021_rf_switch_pins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LR2021_DIO6, RADIOLIB_LR2021_DIO7, RADIOLIB_NC,
    RADIOLIB_NC, RADIOLIB_NC
};

static const Module::RfSwitchMode_t lorawan_lr2021_rf_switch_table[] = {
    { LR2021::MODE_STBY,  {0, 0, 0, 0, 0} },
    { LR2021::MODE_TX,    {0, 0, 0, 0, 0} },
    { LR2021::MODE_RX,    {0, 0, 0, 0, 0} },
    { LR2021::MODE_RX_HF, {1, 0, 0, 0, 0} },
    { LR2021::MODE_TX_HF, {0, 1, 0, 0, 0} },
    END_OF_MODE_TABLE,
};

static void trim(char *text)
{
    char *start;
    char *end;

    if(!text) {
        return;
    }
    start = text;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    end = text + strlen(text);
    while(end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
}

static int hex_value(char c)
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

static int parse_hex_bytes(const char *text, uint8_t *out, size_t expected)
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
        int value = hex_value(text[i]);
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

static int parse_eui(const char *text, uint64_t *out)
{
    uint8_t bytes[8];
    uint64_t value = 0;

    if(!out || !parse_hex_bytes(text, bytes, sizeof(bytes))) {
        return 0;
    }
    for(size_t i = 0; i < sizeof(bytes); i++) {
        value = (value << 8) | bytes[i];
    }
    *out = value;
    return 1;
}

static int parse_bool(const char *text, int fallback)
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

static const lorawan_test_region_t *find_region(const char *name)
{
    if(!name || !name[0]) {
        return &lorawan_test_regions[0];
    }
    for(size_t i = 0; i < sizeof(lorawan_test_regions) /
                           sizeof(lorawan_test_regions[0]); i++) {
        if(!strcasecmp(name, lorawan_test_regions[i].name)) {
            return &lorawan_test_regions[i];
        }
    }
    return NULL;
}

static void config_defaults(lorawan_test_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->region = &lorawan_test_regions[0];
    cfg->fport = 10;
    cfg->adr = 1;
    cfg->datarate = 5;
    cfg->duty_cycle = 1;
    cfg->duty_cycle_ms_per_hour = 1250;
    cfg->dwell_time = 1;
    cfg->dwell_time_ms = 400;
    cfg->tx_power = 14;
}

static int load_config(const char *path, lorawan_test_config_t *cfg)
{
    FILE *fp = fopen(path, "r");
    char line[256];

    config_defaults(cfg);
    if(!fp) {
        fprintf(stderr, "config open failed: %s: %s\n", path, strerror(errno));
        return 0;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *eq;
        char *key = line;
        char *value;

        trim(line);
        if(!line[0] || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if(!eq) {
            continue;
        }
        *eq = '\0';
        value = eq + 1;
        trim(key);
        trim(value);
        if(!strcasecmp(key, "region")) {
            cfg->region = find_region(value);
        } else if(!strcasecmp(key, "sub_band")) {
            cfg->sub_band = (uint8_t)strtoul(value, NULL, 10);
        } else if(!strcasecmp(key, "join_eui") ||
                  !strcasecmp(key, "app_eui")) {
            cfg->has_join_eui = parse_eui(value, &cfg->join_eui);
        } else if(!strcasecmp(key, "dev_eui")) {
            cfg->has_dev_eui = parse_eui(value, &cfg->dev_eui);
        } else if(!strcasecmp(key, "app_key")) {
            cfg->has_app_key = parse_hex_bytes(value, cfg->app_key, 16);
        } else if(!strcasecmp(key, "nwk_key") ||
                  !strcasecmp(key, "nwks_key")) {
            cfg->has_nwk_key = parse_hex_bytes(value, cfg->nwk_key, 16);
        } else if(!strcasecmp(key, "fport")) {
            cfg->fport = (uint8_t)strtoul(value, NULL, 10);
        } else if(!strcasecmp(key, "confirmed")) {
            cfg->confirmed = parse_bool(value, cfg->confirmed);
        } else if(!strcasecmp(key, "adr")) {
            cfg->adr = parse_bool(value, cfg->adr);
        } else if(!strcasecmp(key, "datarate")) {
            cfg->datarate = (uint8_t)strtoul(value, NULL, 10);
        } else if(!strcasecmp(key, "duty_cycle")) {
            cfg->duty_cycle = parse_bool(value, cfg->duty_cycle);
        } else if(!strcasecmp(key, "duty_cycle_ms_per_hour")) {
            cfg->duty_cycle_ms_per_hour = (uint32_t)strtoul(value, NULL, 10);
        } else if(!strcasecmp(key, "dwell_time")) {
            cfg->dwell_time = parse_bool(value, cfg->dwell_time);
        } else if(!strcasecmp(key, "dwell_time_ms")) {
            cfg->dwell_time_ms = (uint32_t)strtoul(value, NULL, 10);
        } else if(!strcasecmp(key, "tx_power")) {
            cfg->tx_power = (int8_t)strtol(value, NULL, 10);
            cfg->has_tx_power = 1;
        }
    }
    fclose(fp);
    if(!cfg->region) {
        fprintf(stderr, "invalid region\n");
        return 0;
    }
    if(!cfg->has_join_eui || !cfg->has_dev_eui ||
       !cfg->has_app_key || !cfg->has_nwk_key) {
        fprintf(stderr, "invalid OTAA keys/eui lengths\n");
        return 0;
    }
    return 1;
}

static void read_binary(const char *path, uint8_t *data, size_t len)
{
    FILE *fp = fopen(path, "rb");
    size_t got;

    if(!fp) {
        return;
    }
    got = fread(data, 1, len, fp);
    if(got < len) {
        memset(data + got, 0, len - got);
    }
    fclose(fp);
}

static void write_binary(const char *path, const uint8_t *data, size_t len)
{
    FILE *fp = fopen(path, "wb");

    if(!fp) {
        return;
    }
    (void)fwrite(data, 1, len, fp);
    fclose(fp);
}

static const char *state_name(int16_t state)
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
        return "RadioLib error";
    }
}

static int begin_sx1262(K230BenchHal *hal, const lorawan_test_config_t *cfg,
                        PhysicalLayer **radio_out, Module **module_out,
                        SX1262 **sx_out)
{
    Module *module = new Module(hal, RADIOLIB_NC, BENCH_PIN_DIO1,
                                BENCH_PIN_RST, BENCH_PIN_BUSY);
    SX1262 *sx = module ? new SX1262(module) : NULL;
    int16_t state;

    if(!module || !sx) {
        delete sx;
        delete module;
        return RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
    }
    state = sx->begin(cfg->region->freq_mhz, 125.0f, 7, 5, 0x34,
                      cfg->has_tx_power ? cfg->tx_power : 14, 8, 3.3f, false);
    if(state == RADIOLIB_ERR_NONE) {
        state = sx->setCRC(0);
    }
    if(state != RADIOLIB_ERR_NONE) {
        delete sx;
        delete module;
        return state;
    }
    *module_out = module;
    *sx_out = sx;
    *radio_out = sx;
    return RADIOLIB_ERR_NONE;
}

static int begin_lr2021(K230BenchHal *hal, const lorawan_test_config_t *cfg,
                        PhysicalLayer **radio_out, Module **module_out,
                        LR2021 **lr_out)
{
    Module *module = new Module(hal, RADIOLIB_NC, BENCH_PIN_DIO1,
                                BENCH_PIN_RST, BENCH_PIN_BUSY);
    LR2021 *lr = module ? new LR2021(module) : NULL;
    int16_t state;

    if(!module || !lr) {
        delete lr;
        delete module;
        return RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
    }
    lr->irqDioNum = BENCH_LR2021_IRQ_DIO_NUM;
    state = lr->begin(cfg->region->freq_mhz, 125.0f, 7, 5, 0x34,
                      cfg->has_tx_power ? cfg->tx_power : 14, 8, 3.0f);
    if(state == RADIOLIB_ERR_NONE) {
        lr->setRfSwitchTable(lorawan_lr2021_rf_switch_pins,
                             lorawan_lr2021_rf_switch_table);
        state = lr->setOutputPower(cfg->has_tx_power ? cfg->tx_power : 14);
    }
    if(state == RADIOLIB_ERR_NONE) {
        state = lr->setCRC(0);
    }
    if(state != RADIOLIB_ERR_NONE) {
        delete lr;
        delete module;
        return state;
    }
    *module_out = module;
    *lr_out = lr;
    *radio_out = lr;
    return RADIOLIB_ERR_NONE;
}

int main(int argc, char **argv)
{
    const char *config_path = LORAWAN_TEST_CONFIG_PATH;
    lorawan_test_config_t cfg;
    K230BenchHal hal(BENCH_SPI_DEV, 4000000U);
    PhysicalLayer *radio = NULL;
    Module *module = NULL;
    SX1262 *sx = NULL;
    LR2021 *lr = NULL;
    lorawan_test_chip_t chip = LORAWAN_TEST_CHIP_NONE;
    uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {0};
    uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {0};
    uint8_t payload[8];
    uint8_t downlink[64];
    size_t downlink_len = 0;
    int16_t state;
    int rc = 1;

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s [--config /root/lorawan/otaa.conf]\n",
                    argv[0]);
            return 2;
        }
    }

    if(!load_config(config_path, &cfg)) {
        return 2;
    }

    printf("CONFIG region=%s sub_band=%u dev_eui=%016llX fport=%u dr=%u confirmed=%d adr=%d\n",
           cfg.region->name, cfg.sub_band, (unsigned long long)cfg.dev_eui,
           cfg.fport, cfg.datarate, cfg.confirmed, cfg.adr);

    hal.pinMode(BENCH_PIN_POWER, K230_HAL_GPIO_OUTPUT);
    hal.digitalWrite(BENCH_PIN_POWER, K230_HAL_GPIO_HIGH);
    hal.delay(30);
    hal.spiBegin();
    if(!hal.spi_ready()) {
        fprintf(stderr, "SPI failed: %s\n", hal.last_error());
        return 3;
    }

    state = begin_sx1262(&hal, &cfg, &radio, &module, &sx);
    if(state == RADIOLIB_ERR_NONE) {
        chip = LORAWAN_TEST_CHIP_SX1262;
    } else {
        printf("PROBE SX1262 failed state=%d %s\n", state, state_name(state));
        state = begin_lr2021(&hal, &cfg, &radio, &module, &lr);
        if(state == RADIOLIB_ERR_NONE) {
            chip = LORAWAN_TEST_CHIP_LR2021;
        } else {
            printf("PROBE LR2021 failed state=%d %s\n", state, state_name(state));
            goto out;
        }
    }
    printf("RADIO chip=%s\n", chip == LORAWAN_TEST_CHIP_SX1262 ? "SX1262" :
           "LR2021");

    {
        LoRaWANNode node(radio, cfg.region->band, cfg.sub_band);

        state = node.beginOTAA(cfg.join_eui, cfg.dev_eui, cfg.nwk_key,
                               cfg.app_key);
        if(state != RADIOLIB_ERR_NONE) {
            printf("OTAA_INIT state=%d %s\n", state, state_name(state));
            goto out_standby;
        }
        read_binary(LORAWAN_TEST_NONCES_PATH, nonces, sizeof(nonces));
        (void)node.setBufferNonces(nonces);
        read_binary(LORAWAN_TEST_SESSION_PATH, session, sizeof(session));
        (void)node.setBufferSession(session);
        node.setADR(cfg.adr != 0);
        node.setDatarate(cfg.datarate);
        node.setDutyCycle(cfg.duty_cycle != 0, cfg.duty_cycle_ms_per_hour);
        node.setDwellTime(cfg.dwell_time != 0, cfg.dwell_time_ms);
        if(cfg.has_tx_power) {
            node.setTxPower(cfg.tx_power);
        }

        printf("JOIN begin\n");
        fflush(stdout);
        state = node.activateOTAA();
        write_binary(LORAWAN_TEST_NONCES_PATH, node.getBufferNonces(),
                     RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
        printf("JOIN state=%d %s\n", state, state_name(state));
        if(state != RADIOLIB_LORAWAN_SESSION_RESTORED &&
           state != RADIOLIB_LORAWAN_NEW_SESSION) {
            goto out_standby;
        }
        write_binary(LORAWAN_TEST_SESSION_PATH, node.getBufferSession(),
                     RADIOLIB_LORAWAN_SESSION_BUF_SIZE);

        memset(payload, 0, sizeof(payload));
        payload[0] = 1;
        payload[1] = 'T';
        payload[2] = 0;
        payload[3] = 225;
        payload[4] = (uint8_t)((time(NULL) >> 24) & 0xFF);
        payload[5] = (uint8_t)((time(NULL) >> 16) & 0xFF);
        payload[6] = (uint8_t)((time(NULL) >> 8) & 0xFF);
        payload[7] = (uint8_t)(time(NULL) & 0xFF);

        printf("SEND begin len=%zu\n", sizeof(payload));
        fflush(stdout);
        state = node.sendReceive(payload, sizeof(payload), cfg.fport,
                                 downlink, &downlink_len, cfg.confirmed != 0);
        write_binary(LORAWAN_TEST_SESSION_PATH, node.getBufferSession(),
                     RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
        printf("SEND state=%d %s fcnt=%lu downlink=%zu\n", state,
               state_name(state), (unsigned long)node.getFCntUp(),
               downlink_len);
        rc = state < RADIOLIB_ERR_NONE ? 4 : 0;
    }

out_standby:
    if(radio) {
        radio->standby();
    }
out:
    delete sx;
    delete lr;
    delete module;
    return rc;
}
