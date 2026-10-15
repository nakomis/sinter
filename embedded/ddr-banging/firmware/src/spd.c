#include <string.h>

#include "spd.h"

uint16_t spd_crc16(const uint8_t *p, unsigned n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

void spd_decode(const uint8_t s[SPD_LEN], spd_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->is_ddr3 = s[2] == 0x0B;

    unsigned crc_len = (s[0] & 0x80) ? 117 : 126;
    o->crc_calc = spd_crc16(s, crc_len);
    o->crc_stored = (uint16_t)(s[126] | s[127] << 8);
    o->crc_ok = o->crc_calc == o->crc_stored;

    o->module_type = s[3] & 0x0F;
    o->chip_mbit = 256u << (s[4] & 0x0F);
    o->banks = 8u << ((s[4] >> 4) & 0x07);
    o->col_bits = 9 + (s[5] & 0x07);
    o->row_bits = 12 + ((s[5] >> 3) & 0x07);
    o->v1_5 = !(s[6] & 0x01);
    o->device_width = 4u << (s[7] & 0x07);
    o->ranks = 1 + ((s[7] >> 3) & 0x07);
    o->bus_width = 8u << (s[8] & 0x07);
    o->ecc = ((s[8] >> 3) & 0x03) == 1;
    o->module_mbyte = o->chip_mbit / 8 * (o->bus_width / o->device_width) * o->ranks;

    // medium timebase in ps (dividend / divisor ns)
    uint32_t mtb_ps = s[11] ? 1000u * s[10] / s[11] : 125;
    o->tck_min_ps = s[12] * mtb_ps;
    o->taa_min_ps = s[16] * mtb_ps;
    o->cas_latencies = (uint16_t)(s[14] | s[15] << 8);

    memcpy(o->part, &s[128], 18);
    o->part[18] = 0;
    for (int i = 17; i >= 0 && (o->part[i] == ' ' || o->part[i] == 0); i--)
        o->part[i] = 0;
}

int spd_tse_sixteenths(uint16_t raw)
{
    int v = raw & 0x0FFF;           // bits 11:0, 1/16 °C
    return (raw & 0x1000) ? v - 0x1000 : v;   // bit 12: sign (13-bit two's complement)
}
