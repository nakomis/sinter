#include <string.h>

#include "check.h"
#include "spd.h"

// A synthetic SPD for the test stick, Samsung M391B5673FH0-CH9: 2 GB 2Rx8
// ECC UDIMM, 1 Gb x8 chips (8 banks, 14 row / 10 column bits), DDR3-1333 CL9.
static void stick(uint8_t s[SPD_LEN])
{
    memset(s, 0, SPD_LEN);
    s[0] = 0x92;          // CRC covers 0..116, 256 bytes total, 176 used
    s[1] = 0x10;          // SPD revision 1.0
    s[2] = 0x0B;          // DDR3
    s[3] = 0x02;          // UDIMM
    s[4] = 0x02;          // 8 banks, 1 Gb
    s[5] = 0x11;          // 14 row bits, 10 column bits
    s[6] = 0x00;          // 1.5 V
    s[7] = 0x09;          // 2 ranks, x8
    s[8] = 0x0B;          // 64-bit primary + 8-bit ECC
    s[10] = 1; s[11] = 8; // MTB = 1/8 ns
    s[12] = 0x0C;         // tCKmin = 12 MTB = 1.5 ns (DDR3-1333)
    s[14] = 0x3E; s[15] = 0x00;  // CL 5..9
    s[16] = 0x6C;         // tAAmin = 108 MTB = 13.5 ns
    memcpy(&s[128], "M391B5673FH0-CH9  ", 18);
    uint16_t crc = spd_crc16(s, 117);
    s[126] = crc & 0xFF;
    s[127] = crc >> 8;
}

static void test_crc_reference_value(void)
{
    // CRC-16/XMODEM (poly 0x1021, init 0), the algorithm in JEDEC 21-C Annex K
    CHECK_EQ(spd_crc16((const uint8_t *)"123456789", 9), 0x31C3);
}

static void test_decode_test_stick(void)
{
    uint8_t s[SPD_LEN];
    stick(s);
    spd_info_t i;
    spd_decode(s, &i);
    CHECK(i.is_ddr3);
    CHECK(i.crc_ok);
    CHECK_EQ(i.module_type, 2);
    CHECK_EQ(i.chip_mbit, 1024);
    CHECK_EQ(i.banks, 8);
    CHECK_EQ(i.row_bits, 14);
    CHECK_EQ(i.col_bits, 10);
    CHECK_EQ(i.device_width, 8);
    CHECK_EQ(i.ranks, 2);
    CHECK_EQ(i.bus_width, 64);
    CHECK(i.ecc);
    CHECK(i.v1_5);
    CHECK_EQ(i.module_mbyte, 2048);
    CHECK_EQ(i.tck_min_ps, 1500);
    CHECK_EQ(i.taa_min_ps, 13500);
    CHECK_EQ(i.cas_latencies, 0x003E);
    CHECK(strcmp(i.part, "M391B5673FH0-CH9") == 0);
}

static void test_bad_crc_detected(void)
{
    uint8_t s[SPD_LEN];
    stick(s);
    s[5] ^= 1;
    spd_info_t i;
    spd_decode(s, &i);
    CHECK(!i.crc_ok);
}

static void test_crc_range_follows_byte0(void)
{
    uint8_t s[SPD_LEN];
    stick(s);
    s[0] &= 0x7F;         // CRC covers 0..125
    uint16_t crc = spd_crc16(s, 126);
    s[126] = crc & 0xFF;
    s[127] = crc >> 8;
    spd_info_t i;
    spd_decode(s, &i);
    CHECK(i.crc_ok);
}

static void test_not_ddr3(void)
{
    uint8_t s[SPD_LEN];
    stick(s);
    s[2] = 0x0C;          // DDR4
    spd_info_t i;
    spd_decode(s, &i);
    CHECK(!i.is_ddr3);
}

void test_spd(void)
{
    RUN(test_crc_reference_value);
    RUN(test_decode_test_stick);
    RUN(test_bad_crc_detected);
    RUN(test_crc_range_follows_byte0);
    RUN(test_not_ddr3);
}
