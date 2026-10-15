// The whole PIO executor (cmd + writer + sampler, PIO0/1/2) under epio,
// against the behavioural DRAM model: bytes written by the writer SM land in
// the model at the right addresses, bytes the model drives come back through
// the sampler SM in bus_run's sample order, and the pin trace meets JEDEC.

#include <string.h>

#include "check.h"
#include "ddr3.h"
#include "dram_model.h"
#include "jedec.h"
#include "pio_enc.h"
#include "pio_sim.h"
#include "sim.h"

static ddr3_cycle_t cyc[8192];
static uint32_t cmd[8192], wr[8192], smp[8192], rx[16384];

static uint32_t real_tck(uint32_t want)
{
    pioenc_timing_t t;
    CHECK(pio_sim_timing(want, &t));
    return (t.tck_ps + 999) / 1000;
}

// Builds one sequence: self-refresh exit, the runs (writes or reads) that
// cover [addr, addr + len), self-refresh entry. Records read runs so the
// samples can be picked apart afterwards, as dram.c does.
static unsigned run_bursts[256], n_runs;

static void build(ddr3_seq_t *s, uint32_t tck, uint32_t addr, const uint8_t *wdata, uint32_t len)
{
    ddr3_seq_init(s, cyc, 8192, tck);
    ddr3_seq_srx(s);
    unsigned per_row = ddr3_seq_row_bursts(s);
    n_runs = 0;
    for (uint32_t j = 0; j < len / 8;) {
        unsigned want = len / 8 - j < per_row ? len / 8 - j : per_row;
        unsigned got = wdata ? ddr3_seq_write_run(s, addr + 8 * j, wdata + 8 * j, want)
                             : ddr3_seq_read_run(s, addr + 8 * j, want);
        run_bursts[n_runs++] = got;
        ddr3_seq_catch_up(s);
        j += got;
    }
    ddr3_seq_sre(s);
}

static bool play(const ddr3_seq_t *s, uint32_t tck, pio_sim_result_t *r)
{
    bool o1, o2, o3;
    pio_sim_streams_t st = {
        .cmd = cmd, .n_cmd = pioenc_cmd(s->cycles, s->len, cmd, 8192, &o1),
        .wr = wr, .n_wr = pioenc_writer(s->cycles, s->len, wr, 8192, &o2),
        .smp = smp, .n_smp = pioenc_sampler(s->cycles, s->len, smp, 8192, &o3),
        .rx = rx, .rx_cap = 16384,
    };
    if (o1 || o2 || o3) return false;
    pioenc_timing_t t;
    pio_sim_timing(tck, &t);
    uint32_t per_ck = PIO_SIM_SM_CYCLES_PER_CK * t.div;
    model_set_ticks_per_ck(per_ck);
    model_rewind();
    sim_input = model_input;
    bool ok = pio_sim_run(&st, tck, (s->len + 4) * per_ck + 64, r);
    sim_input = NULL;
    return ok && r->cmd_used == st.n_cmd && r->wr_used == st.n_wr && r->smp_used == st.n_smp;
}

static void round_trip(uint32_t want_tck)
{
    static uint8_t data[1024], back[1024];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 29 + 7 + (i >> 7));
    const uint32_t at = 0x1F00;   // 256 bytes short of a row end: two rows, two banks
    uint32_t tck = real_tck(want_tck);
    pio_sim_result_t r;
    pioenc_timing_t t;
    pio_sim_timing(want_tck, &t);
    double ns_per_tick = 1e9 / PIO_SIM_SYSCLK_HZ, tck_ns = t.tck_ps / 1000.0;

    // write: everything lands in the model, nothing either side
    model_reset();
    ddr3_seq_t s;
    build(&s, tck, at, data, sizeof data);
    CHECK(!s.overflow);
    CHECK(play(&s, want_tck, &r));
    bool landed = true;
    for (size_t i = 0; i < sizeof data; i++) landed &= model_byte(at + (uint32_t)i) == data[i];
    CHECK(landed);
    CHECK_EQ(model_byte(at - 1), 0);
    CHECK_EQ(model_byte(at + sizeof data), 0);
    CHECK_EQ(r.rx_len, 0);                       // nothing sampled on a write
    CHECK_EQ(jedec_check_pins_ns(ns_per_tick, tck_ns, true), 0);

    // read it back (the model keeps its memory across sim_reset)
    build(&s, tck, at, NULL, sizeof data);
    CHECK(!s.overflow);
    CHECK(play(&s, want_tck, &r));
    CHECK_EQ(jedec_check_pins_ns(ns_per_tick, tck_ns, true), 0);
    // two samples per sampled cycle, in order; burst k of a run starts
    // DDR3_READ_WINDOW_BEFORE + 4k cycles into the run's window
    size_t expect = 0;
    for (unsigned q = 0; q < n_runs; q++) expect += 2 * ddr3_read_run_samples(run_bursts[q]);
    CHECK_EQ(r.rx_len, expect);
    size_t base = 0, o = 0;
    for (unsigned q = 0; q < n_runs; q++) {
        for (unsigned k = 0; k < run_bursts[q]; k++)
            for (int b = 0; b < 8; b++)
                back[o++] = (uint8_t)rx[base + 2 * (DDR3_READ_WINDOW_BEFORE + 4 * k) + b];
        base += 2 * ddr3_read_run_samples(run_bursts[q]);
    }
    CHECK_EQ(o, sizeof data);
    CHECK(memcmp(back, data, sizeof data) == 0);
    // within each burst DQS was high on every first beat and low on every
    // second, as the DRAM drove it (outside a burst it idles low)
    bool strobed = true;
    base = 0;
    for (unsigned q = 0; q < n_runs; q++) {
        size_t w0 = base + 2 * DDR3_READ_WINDOW_BEFORE;
        for (unsigned k = 0; k < 8 * run_bursts[q]; k++)
            strobed &= ((rx[w0 + k] >> 8) & 1) == (k % 2 == 0);
        base += 2 * ddr3_read_run_samples(run_bursts[q]);
    }
    CHECK(strobed);
}

static void test_pio_round_trip_1us(void)   { round_trip(1000); }
static void test_pio_round_trip_250ns(void) { round_trip(250); }
static void test_pio_round_trip_slow(void)  { round_trip(3400); }

void test_pio_data(void)
{
    RUN(test_pio_round_trip_1us);
    RUN(test_pio_round_trip_250ns);
    RUN(test_pio_round_trip_slow);
}
