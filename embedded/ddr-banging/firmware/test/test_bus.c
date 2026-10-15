// bus_gpio.c against a simulated GPIO block (test/stubs): edge order, data
// centring, strobe polarity and translator direction, at quarter-clock
// resolution.

#include <string.h>

#include "bus_gpio.h"
#include "check.h"
#include "pins.h"
#include "stubs/sim.h"

#define G(x, g)   (((x) >> (g)) & 1u)
#define CK_PIN    (PIN_CA_BASE + CA_CK)
#define CKN_PIN   (PIN_CA_BASE + CA_CKN)
#define DQ_OE     (0x3FFull)

static ddr3_cycle_t buf[4096];

static void run(ddr3_seq_t *s, bus_sample_t *samp, unsigned n)
{
    sim_reset();
    bus_run(s, samp, n);
}

// Index of the WR/RD command cycle in a sequence.
static long cmd_index(const ddr3_seq_t *s, ddr3_cmd_t cmd)
{
    uint32_t want = ddr3_encode(cmd, 0, 0, true);
    uint32_t m = (1u << CA_CS0N) | (1u << CA_RASN) | (1u << CA_CASN) | (1u << CA_WEN);
    for (size_t i = 0; i < s->len; i++)
        if ((s->cycles[i].ca & m) == (want & m)) return (long)i;
    return -1;
}

static void test_invariants_and_commands(const ddr3_seq_t *s)
{
    long rises = 0;
    for (size_t i = 0; i < sim_len; i++) {
        sim_snap_t x = sim_log[i];
        CHECK(G(x.out, CK_PIN) != G(x.out, CKN_PIN));
        if (x.oe & (DQ_MASK | DQS_MASK)) {
            // RP2350 only drives with both translators pointing A->B
            CHECK_EQ(x.out & DIR_MASK, DIR_MASK);
        }
        if (x.oe & DQS_MASK) CHECK(G(x.out, PIN_DQS) != G(x.out, PIN_DQSN));
        // command registered on each CK rise is that cycle's word
        if (i && !G(sim_log[i - 1].out, CK_PIN) && G(x.out, CK_PIN)) {
            uint32_t ca = (uint32_t)(x.out >> PIN_CA_BASE) & CA_MASK;
            CHECK((size_t)rises < s->len);
            if ((size_t)rises < s->len) CHECK_EQ(ca, s->cycles[rises].ca & CA_MASK);
            CHECK_EQ(x.t, 4 * rises + 2);
            rises++;
        }
    }
    CHECK_EQ(rises, (long)s->len);
    // released afterwards
    CHECK_EQ(sim_oe & (DQ_MASK | DQS_MASK), 0);
    CHECK_EQ(sim_out & DIR_MASK, 0);
}

static void test_write(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    const uint8_t d[8] = { 0xA5, 0x5A, 0x01, 0x80, 0xFF, 0x00, 0x3C, 0xC3 };
    ddr3_seq_write(&s, 0x2468, d);
    run(&s, NULL, 0);
    test_invariants_and_commands(&s);

    long wr = cmd_index(&s, DDR3_WR);
    long first_rise_t = 4 * (wr + DDR3_CWL) + 2;   // tDQSS = 0

    // DQS edges and driven-DQ changes
    long edges[16], changes[64];
    int ne = 0, nc = 0;
    long driven_from = -1;
    for (size_t i = 1; i < sim_len; i++) {
        sim_snap_t p = sim_log[i - 1], x = sim_log[i];
        if ((x.oe & DQS_MASK) && driven_from < 0) driven_from = x.t;
        if ((x.oe & DQS_MASK) && (p.oe & DQS_MASK) && G(x.out, PIN_DQS) != G(p.out, PIN_DQS) && ne < 16)
            edges[ne++] = x.t;
        if ((x.oe & DQ_MASK) && (p.oe & DQ_MASK) && (x.out & 0xFF) != (p.out & 0xFF) && nc < 64)
            changes[nc++] = x.t;
    }
    CHECK_EQ(ne, 8);
    if (ne != 8) return;
    CHECK_EQ(edges[0], first_rise_t);
    CHECK(edges[0] - driven_from >= 4);            // tWPRE >= 0.9 tCK
    for (int k = 0; k < 8; k++) {
        // DQ at the edge is beat k, and stable from a quarter before to a quarter after
        uint64_t at = 0;
        for (size_t i = 0; i < sim_len && sim_log[i].t <= edges[k]; i++) at = sim_log[i].out;
        CHECK_EQ(at & 0xFF, d[k]);
        for (int c = 0; c < nc; c++)
            CHECK(changes[c] <= edges[k] - 1 || changes[c] >= edges[k] + 1);
    }
    // postamble: DQS stays driven low for >= 0.3 tCK after the last edge
    long released = -1;
    for (size_t i = 1; i < sim_len; i++)
        if ((sim_log[i - 1].oe & DQS_MASK) && !(sim_log[i].oe & DQS_MASK)) released = sim_log[i].t;
    CHECK(released - edges[7] >= 2);
}

static long read_beat0_t;   // time of the first data edge, set per test

static uint64_t dram_drives(long t)
{
    static const uint8_t d[8] = { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87 };
    for (int k = 0; k < 8; k++) {
        long e = read_beat0_t + 2 * k;
        if (t >= e && t < e + 2)
            return d[k] | (uint64_t)(k % 2 == 0) << PIN_DQS | (uint64_t)(k % 2) << PIN_DQSN;
    }
    return 1ull << PIN_DQSN;
}

static void test_read_samples_land_on_beats(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    ddr3_seq_read(&s, 0x1000);
    long rd = cmd_index(&s, DDR3_RD);
    // DLL off: data from the CK edge RL = CL - 1 clocks after READ
    read_beat0_t = 4 * (rd + DDR3_RL_DLL_OFF) + 2;
    bus_sample_t samp[16];
    memset(samp, 0, sizeof samp);
    sim_reset();
    sim_input = dram_drives;
    bus_run(&s, samp, 16);
    test_invariants_and_commands(&s);
    for (size_t i = 0; i < sim_len; i++) CHECK_EQ(sim_log[i].oe & DQ_OE, 0);
    for (int k = 0; k < 8; k++) {
        unsigned beat = 2 * DDR3_READ_WINDOW_BEFORE + k;
        bus_sample_t b = samp[beat / 2];
        CHECK_EQ(b.beat[beat % 2] & 0xFF, 0x10 + 0x11 * k);
        CHECK_EQ((b.beat[beat % 2] >> 8) & 1, k % 2 == 0);   // DQS high on even beats
    }
}

void test_bus(void)
{
    bus_init();
    RUN(test_write);
    RUN(test_read_samples_land_on_beats);
}
