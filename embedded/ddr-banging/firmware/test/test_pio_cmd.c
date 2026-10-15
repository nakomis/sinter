// The command SM (src/pio/ddr3_cmd.pio) under the PIO emulator: for real
// sequences at real clocks, the DRAM must see exactly the sequence's C/A on
// every CK rising edge, one edge per cycle, evenly spaced, and the whole pin
// trace must meet JEDEC.

#include <string.h>

#include "check.h"
#include "ddr3.h"
#include "jedec.h"
#include "pio_enc.h"
#include "pio_sim.h"
#include "pins.h"
#include "sim.h"

static ddr3_cycle_t cyc[8192];
static uint32_t cmd[8192];

#define CA_NO_CK (CA_MASK)   // everything but CK/CK#

// Checks one played sequence against its source; returns rising edges seen.
static size_t check_edges(const ddr3_seq_t *s, uint32_t sysclk_per_ck, int *mismatches, int *bad_gaps)
{
    const uint64_t ck = 1ull << (PIN_CA_BASE + CA_CK), ckn = 1ull << (PIN_CA_BASE + CA_CKN);
    uint64_t prev = 0;
    long last_rise = -1;
    size_t edges = 0;
    *mismatches = *bad_gaps = 0;
    for (size_t i = 0; i < sim_len; i++) {
        uint64_t o = sim_log[i].out;
        if (!(o & ck) != !(o & ckn) == 0) (*bad_gaps)++;   // CK and CK# not complementary
        // tCH = tCL = half a period (JEDEC allows 0.47..0.53 tCK; the SM gives exactly 0.5)
        if (!(o & ck) && (prev & ck) && last_rise >= 0 && 2 * (sim_log[i].t - last_rise) != (long)sysclk_per_ck)
            (*bad_gaps)++;
        if ((o & ck) && !(prev & ck)) {
            uint32_t ca = (uint32_t)(o >> PIN_CA_BASE) & CA_NO_CK;
            if (edges < s->len && ca != (s->cycles[edges].ca & CA_NO_CK)) (*mismatches)++;
            if (last_rise >= 0 && sim_log[i].t - last_rise != (long)sysclk_per_ck) (*bad_gaps)++;
            last_rise = sim_log[i].t;
            edges++;
        }
        prev = o;
    }
    return edges;
}

static void play(ddr3_seq_t *s, uint32_t tck, bool from_sr)
{
    bool ovf;
    size_t w = pioenc_cmd(s->cycles, s->len, cmd, 8192, &ovf);
    CHECK(!ovf);
    pio_sim_result_t r;
    pioenc_timing_t t;
    CHECK(pio_sim_timing(tck, &t));
    uint32_t per_ck = PIO_SIM_SM_CYCLES_PER_CK * t.div;
    // the whole sequence, plus a few cycles' grace to show it stops cleanly
    pio_sim_streams_t st = { .cmd = cmd, .n_cmd = w };
    CHECK(pio_sim_run(&st, tck, (s->len + 4) * per_ck + 64, &r));
    CHECK_EQ(r.cmd_used, w);
    int mism, gaps;
    size_t edges = check_edges(s, per_ck, &mism, &gaps);
    CHECK_EQ(edges, s->len);    // no edge lost, none extra: CK stops when the stream does
    CHECK_EQ(mism, 0);
    CHECK_EQ(gaps, 0);
    // time base: one tick per system clock; tCK as actually run
    CHECK_EQ(jedec_check_pins_ns(1e9 / PIO_SIM_SYSCLK_HZ, t.tck_ps / 1000.0, from_sr), 0);
}

// Sequences are built for the tCK the PIO really runs (the executor reports
// it, as bus_set_tck does now): 1 us comes out at 1013.3 ns.
static uint32_t real_tck(uint32_t want)
{
    pioenc_timing_t t;
    CHECK(pio_sim_timing(want, &t));
    return (t.tck_ps + 999) / 1000;
}

static void test_cmd_sm_plays_init(void)
{
    const uint32_t tcks[] = { 250, 1000 };
    for (int k = 0; k < 2; k++) {
        ddr3_seq_t s;
        ddr3_seq_init(&s, cyc, 8192, real_tck(tcks[k]));
        ddr3_seq_init_mrs(&s);
        ddr3_seq_sre(&s);
        play(&s, tcks[k], false);   // from power-up
    }
}

static void test_cmd_sm_plays_runs(void)
{
    static uint8_t d[8 * 128];
    const uint32_t tcks[] = { 250, 1000, 3400 };
    for (int k = 0; k < 3; k++)
        for (int rd = 0; rd < 2; rd++) {
            ddr3_seq_t s;
            ddr3_seq_init(&s, cyc, 8192, real_tck(tcks[k]));
            ddr3_seq_srx(&s);
            unsigned n = ddr3_seq_row_bursts(&s);
            for (unsigned r = 0; r < 3; r++) {
                if (rd) ddr3_seq_read_run(&s, r << 13, n);
                else ddr3_seq_write_run(&s, r << 13, d, n);
                ddr3_seq_catch_up(&s);
            }
            ddr3_seq_sre(&s);
            CHECK(!s.overflow);
            play(&s, tcks[k], true);
        }
}

void test_pio_cmd(void)
{
    RUN(test_cmd_sm_plays_init);
    RUN(test_cmd_sm_plays_runs);
}
