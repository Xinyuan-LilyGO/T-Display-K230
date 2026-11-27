#include "adsb_decoder.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void decode_hex(const char *hex, uint8_t *out, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        unsigned value = 0;
        assert(sscanf(hex + i * 2U, "%2x", &value) == 1);
        out[i] = (uint8_t)value;
    }
}

int main(void)
{
    uint8_t identification[14];
    uint8_t position[14];
    uint8_t position_odd[14];
    uint8_t velocity[14];
    adsb_frame_t decoded;
    adsb_frame_t even_decoded;
    adsb_frame_t odd_decoded;
    double latitude;
    double longitude;
    char hex[29];

    decode_hex("8D4840D6202CC371C32CE0576098", identification,
               sizeof(identification));
    assert(adsb_modes_crc_valid(identification, sizeof(identification)));
    assert(adsb_decode_frame(identification, sizeof(identification),
                             &decoded) == 0);
    assert(decoded.downlink_format == 17);
    assert(decoded.icao == 0x4840D6U);
    assert(decoded.type_code == 4);
    assert(decoded.message_type == ADSB_MESSAGE_IDENTIFICATION);
    assert(strcmp(decoded.callsign, "KLM1023") == 0);

    adsb_format_hex(identification, sizeof(identification), hex, sizeof(hex));
    assert(strcmp(hex, "8D4840D6202CC371C32CE0576098") == 0);

    decode_hex("8D40621D58C382D690C8AC2863A7", position, sizeof(position));
    assert(adsb_decode_frame(position, sizeof(position), &decoded) == 0);
    assert(decoded.icao == 0x40621DU);
    assert(decoded.message_type == ADSB_MESSAGE_AIRBORNE_POSITION);
    assert(decoded.altitude_valid);
    assert(decoded.altitude_ft == 38000);
    assert(decoded.cpr_odd == 0);
    even_decoded = decoded;

    decode_hex("8D40621D58C386435CC412692AD6", position_odd,
               sizeof(position_odd));
    assert(adsb_decode_frame(position_odd, sizeof(position_odd),
                             &odd_decoded) == 0);
    assert(odd_decoded.cpr_odd == 1);
    assert(adsb_decode_global_cpr(&even_decoded, &odd_decoded, 0,
                                  &latitude, &longitude) == 0);
    assert(fabs(latitude - 52.257202) < 0.00001);
    assert(fabs(longitude - 3.919373) < 0.00001);

    decode_hex("8D485020994409940838175B284F", velocity, sizeof(velocity));
    assert(adsb_decode_frame(velocity, sizeof(velocity), &decoded) == 0);
    assert(decoded.icao == 0x485020U);
    assert(decoded.message_type == ADSB_MESSAGE_AIRBORNE_VELOCITY);
    assert(decoded.velocity_valid);
    assert(fabsf(decoded.speed_knots - 159.2f) < 0.2f);
    assert(fabsf(decoded.track_degrees - 182.9f) < 0.2f);
    assert(decoded.vertical_rate_valid);
    assert(decoded.vertical_rate_fpm == -832);

    identification[5] ^= 0x01U;
    assert(!adsb_modes_crc_valid(identification, sizeof(identification)));
    assert(adsb_decode_frame(identification, sizeof(identification),
                             &decoded) == -1);

    puts("adsb_decoder_test: PASS");
    return 0;
}
