// Every sequence the firmware builds, at every supported clock, against the
// JEDEC timing checker (jedec.c) -- first from the builder's cycle list, then
// from the simulated pins after bus_run and dram.c.

#include <string.h>

#include "bus_gpio.h"
#include "check.h"
#include "ddr3.h"
#include "dram.h"
#include "jedec.h"
#include "sim.h"

static ddr3_cycle_t big[8192];
static const uint32_t tcks[] = { 250, 1000, 2000, 3000, 3400 };   // the shell's range
#define N_TCK (sizeof tcks / sizeof tcks[0])

static void test_checker_catches_violations(void)
{
    ddr3_seq_t s;
    // tRCD: READ one 8 ns clock after ACT
    ddr3_seq_init(&s, big, 8192, 250);
    ddr3_seq_srx(&s);
    ddr3_seq_cmd(&s, DDR3_ACT, 0, 0);
    s.tck_ns = 8;   // re-time the same cycles as if CK were 125 MHz
    CHECK(jedec_check_seq(&s, true) > 0);

    // REF with a row open, then the row left open far past tRAS(max)
    ddr3_seq_init(&s, big, 8192, 1000);
    ddr3_seq_srx(&s);
    ddr3_seq_cmd(&s, DDR3_ACT, 2, 5);
    ddr3_seq_cmd(&s, DDR3_REF, 0, 0);
    for (int k = 0; k < 100; k++) ddr3_seq_cmd(&s, DDR3_NOP, 0, 0);
    ddr3_seq_cmd(&s, DDR3_PRE, 2, 0);
    CHECK(jedec_check_seq(&s, true) >= 2);

    // a command straight after self-refresh exit, inside tXS
    ddr3_seq_init(&s, big, 8192, 1000);
    s.cke = false;
    for (int k = 0; k < 5; k++) ddr3_seq_cmd(&s, DDR3_NOP, 0, 0);
    s.cke = true;
    ddr3_seq_cmd(&s, DDR3_NOP, 0, 0);
    ddr3_seq_cmd(&s, DDR3_ACT, 0, 0);
    CHECK(jedec_check_seq(&s, true) > 0);
}

static void test_init_meets_jedec(void)
{
    for (unsigned k = 0; k < N_TCK; k++) {
        ddr3_seq_t s;
        ddr3_seq_init(&s, big, 8192, tcks[k]);
        ddr3_seq_init_mrs(&s);
        ddr3_seq_sre(&s);
        CHECK(!s.overflow);
        CHECK_EQ(jedec_check_seq(&s, false), 0);
    }
}

static void test_single_bursts_meet_jedec(void)
{
    const uint8_t d[8] = { 0xA5, 1, 2, 3, 4, 5, 6, 7 };
    for (unsigned k = 0; k < N_TCK; k++) {
        ddr3_seq_t s;
        ddr3_seq_init(&s, big, 8192, tcks[k]);
        ddr3_seq_srx(&s);
        ddr3_seq_write(&s, 0x1238, d);
        ddr3_seq_sre(&s);
        ddr3_seq_srx(&s);
        ddr3_seq_read(&s, 0x1238);
        ddr3_seq_sre(&s);
        CHECK(!s.overflow);
        CHECK_EQ(jedec_check_seq(&s, true), 0);
    }
}

static void test_batched_bursts_meet_jedec(void)
{
    const uint8_t d[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    for (unsigned k = 0; k < N_TCK; k++)
        for (int rd = 0; rd < 2; rd++) {
            ddr3_seq_t s;
            ddr3_seq_init(&s, big, 8192, tcks[k]);
            ddr3_seq_srx(&s);
            for (int j = 0; j < 128; j++) {   // across rows and banks: 8 bytes x 128
                if (rd) ddr3_seq_read(&s, 0x3F00u + 8u * j);
                else ddr3_seq_write(&s, 0x3F00u + 8u * j, d);
                ddr3_seq_catch_up(&s);
            }
            ddr3_seq_sre(&s);
            CHECK(!s.overflow);
            CHECK_EQ(jedec_check_seq(&s, true), 0);
        }
}

// The whole firmware path: dram.c builds, bus_gpio.c plays it on the
// (simulated) pins, and the checker reads the pins back.
static void test_dram_on_the_pins_meets_jedec(void)
{
    static uint8_t data[4096], back[4096];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7);
    const uint32_t try[] = { 1000, 3400 };
    for (int k = 0; k < 2; k++) {
        sim_reset();
        bus_init();
        bus_set_tck(try[k]);
        sim_reset();             // check from here: the calibration runs aren't DRAM traffic
        CHECK(dram_init());
        CHECK(dram_write(0x8000, data, sizeof data));
        CHECK(dram_read(0x8000, back, sizeof back));
        CHECK(sim_len < SIM_MAX);
        CHECK_EQ(jedec_check_pins(bus_tck(), false), 0);
    }
}

void test_jedec(void)
{
    RUN(test_checker_catches_violations);
    RUN(test_init_meets_jedec);
    RUN(test_single_bursts_meet_jedec);
    RUN(test_batched_bursts_meet_jedec);
    RUN(test_dram_on_the_pins_meets_jedec);
}
