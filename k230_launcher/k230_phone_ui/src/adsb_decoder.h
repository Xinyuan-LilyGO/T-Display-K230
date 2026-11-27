#ifndef K230_ADSB_DECODER_H
#define K230_ADSB_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ADSB_MESSAGE_OTHER = 0,
    ADSB_MESSAGE_IDENTIFICATION,
    ADSB_MESSAGE_AIRBORNE_POSITION,
    ADSB_MESSAGE_AIRBORNE_VELOCITY,
} adsb_message_type_t;

typedef struct {
    uint8_t raw[14];
    size_t raw_len;
    uint8_t downlink_format;
    uint8_t capability;
    uint8_t type_code;
    uint8_t subtype;
    uint32_t icao;
    adsb_message_type_t message_type;
    char callsign[9];
    int altitude_valid;
    int altitude_ft;
    int cpr_odd;
    uint32_t cpr_lat;
    uint32_t cpr_lon;
    int velocity_valid;
    float speed_knots;
    float track_degrees;
    int vertical_rate_valid;
    int vertical_rate_fpm;
} adsb_frame_t;

uint32_t adsb_modes_crc(const uint8_t *frame, size_t len);
int adsb_modes_crc_valid(const uint8_t *frame, size_t len);
int adsb_decode_frame(const uint8_t *frame, size_t len, adsb_frame_t *decoded);
int adsb_decode_global_cpr(const adsb_frame_t *even_frame,
                           const adsb_frame_t *odd_frame, int use_odd,
                           double *latitude, double *longitude);
void adsb_format_hex(const uint8_t *frame, size_t len, char *out,
                     size_t out_len);

#endif
