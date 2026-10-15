// The 'selftest' check (src/pio_check.c) run under epio, as the firmware
// will run it on the Core2350B with nothing attached: the capture of GPIO
// 0-31 matches the expected pins on every SM cycle, and the sampler reads
// the writer's bursts back through the pads.

#include <string.h>

#include "check.h"
#include "pio_check.h"
#include "pio_enc.h"
#include "pio_sim.h"
#include "sim.h"

static ddr3_cycle_t cyc[PIO_CHECK_CYCLES];
static uint32_t cmd[256], wr[256], smp[256], rx[256], cap[PIO_CHECK_CAPTURE];
static bus_sample_t samples[64];

static bool run_check(uint32_t tck, pio_check_result_t *res)
{
    size_t n = pio_check_sequence(cyc, PIO_CHECK_CYCLES);
    bool o1, o2, o3;
    pio_sim_streams_t st = {
        .cmd = cmd, .n_cmd = pioenc_cmd(cyc, n, cmd, 256, &o1),
        .wr = wr, .n_wr = pioenc_writer(cyc, n, wr, 256, &o2),
        .smp = smp, .n_smp = pioenc_sampler(cyc, n, smp, 256, &o3),
        .rx = rx, .rx_cap = 256, .capture = cap, .capture_cap = PIO_CHECK_CAPTURE,
    };
    CHECK(!o1 && !o2 && !o3);
    pioenc_timing_t t;
    pio_sim_timing(tck, &t);
    pio_sim_result_t r;
    sim_input = NULL;   // nothing attached: the pads read back the RP2350's own pins
    CHECK(pio_sim_run(&st, tck, (n + 4) * PIO_SIM_SM_CYCLES_PER_CK * t.div + 64, &r));
    CHECK_EQ(r.capture_len, PIO_CHECK_CAPTURE);
    for (size_t j = 0; j < r.rx_len / 2 && j < 64; j++) {
        samples[j].beat[0] = (uint16_t)rx[2 * j];
        samples[j].beat[1] = (uint16_t)rx[2 * j + 1];
    }
    return pio_check(cyc, n, cap, r.capture_len, samples, r.rx_len / 2, res);
}

static void test_selftest_passes_under_epio(void)
{
    const uint32_t tcks[] = { 1000, 250, 3400 };
    for (int k = 0; k < 3; k++) {
        pio_check_result_t res;
        CHECK(run_check(tcks[k], &res));
        CHECK_EQ(res.pin_errors, 0);
        CHECK_EQ(res.loop_errors, 0);
        CHECK_EQ(res.samples, 2 * 16);   // every burst (16..31) has a driving successor
        CHECK(res.offset >= 0 && res.offset < 8);
    }
}

static void test_selftest_notices_a_wrong_pin(void)
{
    pio_check_result_t res;
    CHECK(run_check(1000, &res));
    // one bit wrong for one SM cycle, mid-burst: caught, and pinned down
    size_t at = (size_t)res.offset + 8 * 20 + 3;
    cap[at] ^= 1u << 5;   // DQ5
    size_t n = pio_check_sequence(cyc, PIO_CHECK_CYCLES);
    CHECK(!pio_check(cyc, n, cap, PIO_CHECK_CAPTURE, samples, 16, &res));
    CHECK_EQ(res.pin_errors, 1);
    CHECK_EQ(res.first_bad, 8 * 20 + 3);
    cap[at] ^= 1u << 5;
    // a sampled beat wrong: caught
    samples[3].beat[1] ^= 1;
    CHECK(!pio_check(cyc, n, cap, PIO_CHECK_CAPTURE, samples, 16, &res));
    CHECK_EQ(res.loop_errors, 1);
    samples[3].beat[1] ^= 1;
    // no clock at all: caught
    static uint32_t flat[PIO_CHECK_CAPTURE];
    CHECK(!pio_check(cyc, n, flat, PIO_CHECK_CAPTURE, samples, 16, &res));
    CHECK_EQ(res.offset, -1);
}

void test_pio_selftest(void)
{
    RUN(test_selftest_passes_under_epio);
    RUN(test_selftest_notices_a_wrong_pin);
}
