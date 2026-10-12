// DDR3 SPD (JEDEC 21-C Annex K) decoding. Pure logic, host-tested.

#ifndef SPD_H
#define SPD_H

#include <stdbool.h>
#include <stdint.h>

#define SPD_LEN 256

typedef struct {
    bool     is_ddr3;
    bool     crc_ok;
    uint16_t crc_stored, crc_calc;
    uint8_t  module_type;      // byte 3 [3:0]: 1 RDIMM, 2 UDIMM, 3 SO-DIMM, ...
    uint32_t chip_mbit;        // per SDRAM
    unsigned banks;
    unsigned row_bits, col_bits;
    unsigned device_width;     // x4/x8/x16/x32
    unsigned ranks;
    unsigned bus_width;        // primary, bits
    bool     ecc;
    bool     v1_5;             // 1.5 V operable
    uint32_t module_mbyte;
    uint32_t tck_min_ps;       // fastest clock
    uint32_t taa_min_ps;
    uint16_t cas_latencies;    // bit n = CL(n+4) supported
    char     part[19];         // bytes 128..145, trailing spaces trimmed
} spd_info_t;

uint16_t spd_crc16(const uint8_t *p, unsigned n);

// On-module thermal sensor (JEDEC TSE2002, raw cards D and E), register 5.
#define TSE_I2C_ADDR 0x18
#define TSE_REG_TEMP 0x05
// Temperature in 1/16 °C from the 16-bit register value (flag bits ignored).
int spd_tse_sixteenths(uint16_t raw);
void spd_decode(const uint8_t raw[SPD_LEN], spd_info_t *out);

#endif
