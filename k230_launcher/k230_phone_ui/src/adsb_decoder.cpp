#include "adsb_decoder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MODES_CRC_POLY 0xFFF409U

static uint32_t adsb_get_bits(const uint8_t *data, unsigned first,
                              unsigned count)
{
    uint32_t value = 0;

    for(unsigned i = 0; i < count; i++) {
        unsigned bit = first + i;
        value = (value << 1U) |
                ((data[bit / 8U] >> (7U - (bit % 8U))) & 1U);
    }
    return value;
}

uint32_t adsb_modes_crc(const uint8_t *frame, size_t len)
{
    uint32_t remainder = 0;

    if(!frame || (len != 7U && len != 14U)) {
        return UINT32_MAX;
    }

    for(size_t i = 0; i < len * 8U; i++) {
        uint32_t input = (frame[i / 8U] >> (7U - (i % 8U))) & 1U;
        uint32_t top = (remainder >> 23U) & 1U;
        remainder = ((remainder << 1U) & 0xFFFFFFU) | input;
        if(top) {
            remainder ^= MODES_CRC_POLY;
        }
    }
    return remainder;
}

int adsb_modes_crc_valid(const uint8_t *frame, size_t len)
{
    return adsb_modes_crc(frame, len) == 0U;
}

static void adsb_decode_callsign(const uint8_t *frame, char callsign[9])
{
    static const char charset[] =
        "#ABCDEFGHIJKLMNOPQRSTUVWXYZ#####_###############0123456789######";
    size_t end = 8U;

    for(unsigned i = 0; i < 8U; i++) {
        uint32_t code = adsb_get_bits(frame, 40U + i * 6U, 6U);
        char ch = code < sizeof(charset) - 1U ? charset[code] : '#';
        callsign[i] = ch == '_' ? ' ' : ch;
    }
    callsign[8] = '\0';
    while(end > 0U && callsign[end - 1U] == ' ') {
        callsign[--end] = '\0';
    }
}

static void adsb_decode_altitude(const uint8_t *frame, adsb_frame_t *decoded)
{
    uint16_t encoded = (uint16_t)adsb_get_bits(frame, 40U, 12U);

    if(encoded & 0x10U) {
        uint16_t n = (uint16_t)(((encoded & 0x0FE0U) >> 1U) |
                                (encoded & 0x000FU));
        decoded->altitude_ft = (int)n * 25 - 1000;
        decoded->altitude_valid = 1;
    }
}

static void adsb_decode_velocity(const uint8_t *frame, adsb_frame_t *decoded)
{
    unsigned subtype = (unsigned)adsb_get_bits(frame, 37U, 3U);
    unsigned scale = subtype == 2U ? 4U : 1U;
    unsigned ew_raw;
    unsigned ns_raw;
    int ew;
    int ns;

    decoded->subtype = (uint8_t)subtype;
    if(subtype != 1U && subtype != 2U) {
        return;
    }

    ew_raw = (unsigned)adsb_get_bits(frame, 46U, 10U);
    ns_raw = (unsigned)adsb_get_bits(frame, 57U, 10U);
    if(ew_raw == 0U || ns_raw == 0U) {
        return;
    }
    ew = (int)(ew_raw - 1U) * (int)scale;
    ns = (int)(ns_raw - 1U) * (int)scale;
    if(adsb_get_bits(frame, 45U, 1U)) {
        ew = -ew;
    }
    if(adsb_get_bits(frame, 56U, 1U)) {
        ns = -ns;
    }

    decoded->speed_knots = sqrtf((float)(ew * ew + ns * ns));
    decoded->track_degrees = atan2f((float)ew, (float)ns) *
                             (180.0f / 3.14159265358979323846f);
    if(decoded->track_degrees < 0.0f) {
        decoded->track_degrees += 360.0f;
    }
    decoded->velocity_valid = 1;

    unsigned vr_raw = (unsigned)adsb_get_bits(frame, 69U, 9U);
    if(vr_raw != 0U) {
        int vertical_rate = (int)(vr_raw - 1U) * 64;
        if(adsb_get_bits(frame, 68U, 1U)) {
            vertical_rate = -vertical_rate;
        }
        decoded->vertical_rate_fpm = vertical_rate;
        decoded->vertical_rate_valid = 1;
    }
}

int adsb_decode_frame(const uint8_t *frame, size_t len, adsb_frame_t *decoded)
{
    if(!frame || !decoded || len != 14U || !adsb_modes_crc_valid(frame, len)) {
        return -1;
    }

    memset(decoded, 0, sizeof(*decoded));
    memcpy(decoded->raw, frame, len);
    decoded->raw_len = len;
    decoded->downlink_format = (uint8_t)adsb_get_bits(frame, 0U, 5U);
    decoded->capability = (uint8_t)adsb_get_bits(frame, 5U, 3U);
    decoded->icao = adsb_get_bits(frame, 8U, 24U);
    if(decoded->downlink_format != 17U && decoded->downlink_format != 18U) {
        return -2;
    }

    decoded->type_code = (uint8_t)adsb_get_bits(frame, 32U, 5U);
    if(decoded->type_code >= 1U && decoded->type_code <= 4U) {
        decoded->message_type = ADSB_MESSAGE_IDENTIFICATION;
        adsb_decode_callsign(frame, decoded->callsign);
    } else if((decoded->type_code >= 9U && decoded->type_code <= 18U) ||
              (decoded->type_code >= 20U && decoded->type_code <= 22U)) {
        decoded->message_type = ADSB_MESSAGE_AIRBORNE_POSITION;
        adsb_decode_altitude(frame, decoded);
        decoded->cpr_odd = (int)adsb_get_bits(frame, 53U, 1U);
        decoded->cpr_lat = adsb_get_bits(frame, 54U, 17U);
        decoded->cpr_lon = adsb_get_bits(frame, 71U, 17U);
    } else if(decoded->type_code == 19U) {
        decoded->message_type = ADSB_MESSAGE_AIRBORNE_VELOCITY;
        adsb_decode_velocity(frame, decoded);
    } else {
        decoded->message_type = ADSB_MESSAGE_OTHER;
    }
    return 0;
}

static int adsb_positive_mod(int value, int modulus)
{
    int result = value % modulus;
    return result < 0 ? result + modulus : result;
}

static int adsb_cpr_nl(double latitude)
{
    double latitude_rad;
    double denominator;

    latitude = fabs(latitude);
    if(latitude >= 87.0) {
        return 1;
    }
    if(latitude < 1e-9) {
        return 59;
    }
    latitude_rad = latitude * (3.14159265358979323846 / 180.0);
    denominator = cos(latitude_rad);
    denominator *= denominator;
    return (int)floor((2.0 * 3.14159265358979323846) /
                      acos(1.0 - (1.0 - cos(3.14159265358979323846 / 30.0)) /
                           denominator));
}

int adsb_decode_global_cpr(const adsb_frame_t *even_frame,
                           const adsb_frame_t *odd_frame, int use_odd,
                           double *latitude, double *longitude)
{
    double lat_even;
    double lat_odd;
    double lon_even;
    double lon_odd;
    double rlat_even;
    double rlat_odd;
    double selected_lat;
    double selected_lon;
    int latitude_index;
    int longitude_index;
    int nl;
    int ni;

    if(!even_frame || !odd_frame || !latitude || !longitude ||
       even_frame->message_type != ADSB_MESSAGE_AIRBORNE_POSITION ||
       odd_frame->message_type != ADSB_MESSAGE_AIRBORNE_POSITION ||
       even_frame->cpr_odd || !odd_frame->cpr_odd ||
       even_frame->icao != odd_frame->icao) {
        return -1;
    }

    lat_even = (double)even_frame->cpr_lat / 131072.0;
    lat_odd = (double)odd_frame->cpr_lat / 131072.0;
    lon_even = (double)even_frame->cpr_lon / 131072.0;
    lon_odd = (double)odd_frame->cpr_lon / 131072.0;
    latitude_index = (int)floor(59.0 * lat_even - 60.0 * lat_odd + 0.5);
    rlat_even = 6.0 * (adsb_positive_mod(latitude_index, 60) + lat_even);
    rlat_odd = (360.0 / 59.0) *
               (adsb_positive_mod(latitude_index, 59) + lat_odd);
    if(rlat_even >= 270.0) {
        rlat_even -= 360.0;
    }
    if(rlat_odd >= 270.0) {
        rlat_odd -= 360.0;
    }
    if(adsb_cpr_nl(rlat_even) != adsb_cpr_nl(rlat_odd)) {
        return -2;
    }

    nl = adsb_cpr_nl(use_odd ? rlat_odd : rlat_even);
    longitude_index = (int)floor(lon_even * (nl - 1) - lon_odd * nl + 0.5);
    ni = nl - (use_odd ? 1 : 0);
    if(ni < 1) {
        ni = 1;
    }
    selected_lon = (360.0 / ni) *
                   (adsb_positive_mod(longitude_index, ni) +
                    (use_odd ? lon_odd : lon_even));
    if(selected_lon > 180.0) {
        selected_lon -= 360.0;
    }
    selected_lat = use_odd ? rlat_odd : rlat_even;
    if(selected_lat < -90.0 || selected_lat > 90.0) {
        return -3;
    }
    *latitude = selected_lat;
    *longitude = selected_lon;
    return 0;
}

void adsb_format_hex(const uint8_t *frame, size_t len, char *out,
                     size_t out_len)
{
    size_t used = 0;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(!frame) {
        return;
    }
    for(size_t i = 0; i < len && used + 2U < out_len; i++) {
        int written = snprintf(out + used, out_len - used, "%02X", frame[i]);
        if(written != 2) {
            break;
        }
        used += 2U;
    }
}
