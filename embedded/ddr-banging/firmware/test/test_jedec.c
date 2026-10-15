// Every sequence the firmware builds, at every supported clock, against the
// JEDEC timing checker (jedec.c) -- first from the builder's cycle list, then
// from the simulated pins after bus_run and dram.c.

#include <string.h>

#include "bus_gpio.h"
#include "check.h"
#include "ddr3.h"
#include "dram.h"
#include "dram_model.h"
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

// Runs: as many bursts per open row as refresh allows, at every clock
static void test_row_runs_meet_jedec(void)
{
    static uint8_t d[8 * 128];
    for (size_t i = 0; i < sizeof d; i++) d[i] = (uint8_t)i;
    for (unsigned k = 0; k < N_TCK; k++)
        for (int rd = 0; rd < 2; rd++) {
            ddr3_seq_t s;
            ddr3_seq_init(&s, big, 8192, tcks[k]);
            ddr3_seq_srx(&s);
            unsigned n = ddr3_seq_row_bursts(&s);
            CHECK(n >= 1);
            for (unsigned r = 0; r < 3; r++) {   // three runs, each ending its own row
                uint32_t a = (r << 13) | 3u << 10 | (1024u - 8u * n);
                if (rd) ddr3_seq_read_run(&s, a, n);
                else ddr3_seq_write_run(&s, a, d, n);
            }
            ddr3_seq_sre(&s);
            CHECK(!s.overflow);
            CHECK_EQ(jedec_check_seq(&s, true), 0);
        }
    // a run asked to cross a row stops at its end: no auto-precharge bit
    ddr3_seq_t s;
    ddr3_seq_init(&s, big, 8192, 250);
    ddr3_seq_srx(&s);
    ddr3_seq_write_run(&s, 1024 - 16, d, 10);
    ddr3_seq_sre(&s);
    CHECK_EQ(jedec_check_seq(&s, true), 0);
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

// Bytes in, bytes out: dram.c through bus_run into the data model and back,
// across row and bank boundaries, at the clock extremes.
static void test_dram_round_trip_through_the_model(void)
{
    static uint8_t data[3000], back[3000];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 37 + (i >> 8));
    const uint32_t try[] = { 250, 1000, 3400 };
    const uint32_t at = 0x1FC0;   // 64 bytes short of the end of row 0, bank 7
    for (int k = 0; k < 3; k++) {
        sim_reset();
        bus_init();
        bus_set_tck(try[k]);
        sim_reset();
        model_reset();
        sim_input = model_input;
        CHECK(dram_init());
        CHECK(dram_write(at, data, sizeof data));
        bool landed = true;
        for (size_t i = 0; i < sizeof data; i++) landed &= model_byte(at + (uint32_t)i) == data[i];
        CHECK(landed);
        CHECK_EQ(model_byte(at - 1), 0);
        CHECK_EQ(model_byte(at + sizeof data), 0);
        memset(back, 0, sizeof back);
        CHECK(dram_read(at, back, sizeof back));
        CHECK(memcmp(back, data, sizeof data) == 0);
        uint8_t one[8];
        CHECK(dram_read_burst(at + 1000, one));
        CHECK(memcmp(one, data + 1000, 8) == 0);
        CHECK(!model_out_of_range());
        CHECK(sim_len < SIM_MAX);
        sim_input = NULL;
    }
}

// Hot: every sequence again at the 2x refresh rate, up to the hot tCK cap
static void test_hot_sequences_meet_jedec(void)
{
    static uint8_t d[8 * 128];
    const uint32_t hot_tcks[] = { 250, 1000, 1700 };
    jedec_set_hot(true);
    for (int k = 0; k < 3; k++)
        for (int rd = 0; rd < 2; rd++) {
            ddr3_seq_t s;
            ddr3_seq_init(&s, big, 8192, hot_tcks[k]);
            ddr3_seq_set_hot(&s, true);
            ddr3_seq_srx(&s);
            unsigned n = ddr3_seq_row_bursts(&s);
            for (unsigned r = 0; r < 6; r++) {
                uint32_t a = (r << 13) | (1024u - 8u * n);
                if (rd) ddr3_seq_read_run(&s, a, n);
                else ddr3_seq_write_run(&s, a, d, n);
            }
            ddr3_seq_sre(&s);
            CHECK(!s.overflow);
            CHECK_EQ(jedec_check_seq(&s, true), 0);
        }
    jedec_set_hot(false);
}

static int fake_temp16;
static bool have_sensor;
static bool fake_thermometer(int *t16)
{
    *t16 = fake_temp16;
    return have_sensor;
}

static void start_sim(uint32_t tck)
{
    sim_reset();
    bus_init();
    bus_set_tck(tck);
    sim_reset();
    model_reset();
    sim_input = model_input;
}

// dram.c on a warm bench: switches to 2x refresh with hysteresis, caps tCK,
// and refuses to run hot on a stick without the extended range
static void test_dram_follows_the_temperature(void)
{
    static uint8_t data[2048], back[2048];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i ^ 0x5A);
    dram_set_thermometer(fake_thermometer);
    have_sensor = true;

    // extended range, hot at tCK 3.4 us: clock capped, 2x schedule on the pins
    dram_set_extended_temp(true);
    CHECK(ddr3_mr2() & 0x80);
    fake_temp16 = 81 * 16;
    start_sim(3400);
    CHECK(dram_init());
    CHECK(dram_hot());
    CHECK(bus_tck() <= DDR3_TCK_MAX_HOT_NS);
    CHECK(dram_write(0x400, data, sizeof data));
    CHECK(dram_read(0x400, back, sizeof back));
    CHECK(memcmp(back, data, sizeof data) == 0);
    jedec_set_hot(true);
    CHECK_EQ(jedec_check_pins(bus_tck(), false), 0);
    jedec_set_hot(false);

    // hysteresis: 79 C stays hot, 77 C is cool again
    fake_temp16 = 79 * 16;
    CHECK(dram_read(0x400, back, 8));
    CHECK(dram_hot());
    fake_temp16 = 77 * 16;
    CHECK(dram_read(0x400, back, 8));
    CHECK(!dram_hot());

    // no extended range: hot means stop
    dram_set_extended_temp(false);
    CHECK(!(ddr3_mr2() & 0x80));
    fake_temp16 = 82 * 16;
    CHECK(!dram_read(0x400, back, 8));
    CHECK(dram_error() != NULL);

    // a failed read while hot stays hot
    have_sensor = false;
    CHECK(!dram_read(0x400, back, 8));
    CHECK(dram_hot());

    // no sensor at all: cool
    dram_set_thermometer(fake_thermometer);
    fake_temp16 = 99 * 16;
    CHECK(dram_read(0x400, back, 8));
    CHECK(!dram_hot());

    dram_set_thermometer(NULL);
    sim_input = NULL;
}

// dram.c builds for, and runs on, whichever executor is selected
static unsigned fake_runs;
static uint32_t fake_tck_seen;
static bool fake_ok;
static bool fake_run(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max)
{
    (void)samples, (void)max;
    fake_runs++;
    fake_tck_seen = s->tck_ns;
    return fake_ok;
}
static uint32_t fake_tck(void) { return 1234; }
static uint32_t fake_set_tck(uint32_t ns) { (void)ns; return 1234; }
static const bus_ops_t fake_bus = { "fake", fake_run, fake_tck, fake_set_tck };

static void test_dram_uses_the_selected_bus(void)
{
    uint8_t b[16] = { 0 };
    CHECK(dram_bus() == &bus_gpio_ops);   // the default
    dram_set_bus(&fake_bus);
    fake_runs = 0;
    fake_ok = true;
    CHECK(dram_write(0, b, 16));
    CHECK_EQ(fake_runs, 1);
    CHECK_EQ(fake_tck_seen, 1234);        // sequences built for its clock
    fake_ok = false;
    CHECK(!dram_read(0, b, 16));          // its failure is the operation's
    CHECK(dram_error() != NULL);
    dram_set_bus(&bus_gpio_ops);
}

void test_jedec(void)
{
    RUN(test_checker_catches_violations);
    RUN(test_init_meets_jedec);
    RUN(test_single_bursts_meet_jedec);
    RUN(test_batched_bursts_meet_jedec);
    RUN(test_row_runs_meet_jedec);
    RUN(test_dram_on_the_pins_meets_jedec);
    RUN(test_dram_round_trip_through_the_model);
    RUN(test_hot_sequences_meet_jedec);
    RUN(test_dram_follows_the_temperature);
    RUN(test_dram_uses_the_selected_bus);
}
