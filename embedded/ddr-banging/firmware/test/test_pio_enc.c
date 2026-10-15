// The PIO executor's word streams (src/pio_enc.c): every sequence the
// firmware builds survives encode/decode exactly, compresses its idle
// stretches, and still meets JEDEC when played back from the decoded form.

#include <string.h>

#include "check.h"
#include "ddr3.h"
#include "jedec.h"
#include "pio_enc.h"

static ddr3_cycle_t seq_cyc[8192], back[8192];
static uint32_t cmd[8192], wr[8192], smp[8192];
static size_t nc, nw, ns;

static bool encode_all(const ddr3_cycle_t *c, size_t n)
{
    bool o1, o2, o3;
    nc = pioenc_cmd(c, n, cmd, 8192, &o1);
    nw = pioenc_writer(c, n, wr, 8192, &o2);
    ns = pioenc_sampler(c, n, smp, 8192, &o3);
    return !o1 && !o2 && !o3;
}

static bool round_trips(const ddr3_seq_t *s)
{
    if (!encode_all(s->cycles, s->len)) return false;
    size_t n = pioenc_decode(cmd, nc, wr, nw, smp, ns, back, 8192);
    if (n != s->len) return false;
    for (size_t i = 0; i < n; i++) {
        ddr3_cycle_t a = pioenc_normalise(s->cycles[i]), b = back[i];
        if (a.ca != b.ca || a.flags != b.flags || a.dq[0] != b.dq[0] || a.dq[1] != b.dq[1])
            return false;
    }
    return true;
}

static void test_real_sequences_round_trip(void)
{
    static uint8_t d[8 * 128];
    for (size_t i = 0; i < sizeof d; i++) d[i] = (uint8_t)(i * 13 + 1);
    const uint32_t tcks[] = { 250, 1000, 3400 };
    for (int k = 0; k < 3; k++) {
        ddr3_seq_t s;
        // init: 512 CK of tZQinit NOPs collapse; writer and sampler are idle throughout
        ddr3_seq_init(&s, seq_cyc, 8192, tcks[k]);
        ddr3_seq_init_mrs(&s);
        ddr3_seq_sre(&s);
        CHECK(round_trips(&s));
        CHECK(nc * 4 < s.len);
        CHECK_EQ(nw, 1);
        CHECK_EQ(ns, 1);
        // a batch of write runs and of read runs, as dram.c builds them
        for (int rd = 0; rd < 2; rd++) {
            ddr3_seq_init(&s, seq_cyc, 8192, tcks[k]);
            ddr3_seq_srx(&s);
            unsigned n = ddr3_seq_row_bursts(&s);
            for (unsigned r = 0; r < 4; r++) {
                if (rd) ddr3_seq_read_run(&s, r << 13, n);
                else ddr3_seq_write_run(&s, r << 13, d, n);
                ddr3_seq_catch_up(&s);
            }
            ddr3_seq_sre(&s);
            CHECK(!s.overflow);
            CHECK(round_trips(&s));
            CHECK(nc < s.len && nw < s.len && ns < s.len);
            // the executor's reference: what comes out of decode still meets JEDEC
            ddr3_seq_t played = s;
            played.cycles = back;
            CHECK_EQ(jedec_check_seq(&played, true), 0);
        }
    }
}

static void test_runs_split_at_the_repeat_limit(void)
{
    ddr3_cycle_t nop = { .ca = ddr3_encode(DDR3_NOP, 0, 0, true) };
    for (int i = 0; i < 2000; i++) seq_cyc[i] = nop;
    bool ovf;
    size_t w = pioenc_cmd(seq_cyc, 2000, cmd, 8192, &ovf);
    CHECK_EQ(w, 4);                                   // 512 + 512 + 512 + 464
    CHECK_EQ(cmd[0] >> PIOENC_REP_SHIFT, 511);
    CHECK_EQ(cmd[3] >> PIOENC_REP_SHIFT, 463);
    CHECK_EQ(pioenc_writer(seq_cyc, 2000, wr, 8192, &ovf), 1);   // 16-bit repeats
    CHECK_EQ(wr[0], 1999u << 2);
    CHECK(encode_all(seq_cyc, 2000));
    CHECK_EQ(pioenc_decode(cmd, nc, wr, nw, smp, ns, back, 8192), 2000);
    CHECK_EQ(pioenc_cmd(seq_cyc, 2000, cmd, 2, &ovf), 2);
    CHECK(ovf);
}

static void test_layout_and_disagreement(void)
{
    ddr3_cycle_t c = { .ca = ddr3_encode(DDR3_ACT, 5, 0x2ABC, true) | (1u << CA_CK),
                       .flags = CY_DRIVE | CY_STROBE, .dq = { 0xA5, 0x3C } };
    bool ovf;
    CHECK_EQ(pioenc_cmd(&c, 1, cmd, 4, &ovf), 1);
    CHECK_EQ(cmd[0], (c.ca >> CA_CKE) & 0x7FFFFFu);   // CK dropped, C/A from GPIO 18
    CHECK_EQ(pioenc_writer(&c, 1, wr, 4, &ovf), 1);
    CHECK_EQ(wr[0], PIOENC_WR_STROBE | 0xA5u << 2 | 0x3Cu << 10);
    // drive words never repeat: their high bits are data
    ddr3_cycle_t pre[2] = { { .flags = CY_DRIVE }, { .flags = CY_DRIVE } };
    CHECK_EQ(pioenc_writer(pre, 2, wr, 4, &ovf), 2);
    CHECK_EQ(wr[0], PIOENC_WR_DRIVE);
    // write data that isn't driven doesn't split an idle run
    ddr3_cycle_t idle[2] = { { .ca = 0, .dq = { 1, 2 } }, { .ca = 0, .dq = { 3, 4 } } };
    CHECK_EQ(pioenc_writer(idle, 2, wr, 4, &ovf), 1);
    // a sampled cycle marks the next one's beat 1, splitting a run of sampling once
    ddr3_cycle_t s4[4] = { { .flags = CY_SAMPLE }, { .flags = CY_SAMPLE }, { .flags = CY_SAMPLE }, { 0 } };
    CHECK_EQ(pioenc_sampler(s4, 4, smp, 8, &ovf), 3);
    CHECK_EQ(smp[0], PIOENC_SMP_BEAT0);
    CHECK_EQ(smp[1], PIOENC_SMP_BEAT0 | PIOENC_SMP_BEAT1 | 1u << 2);
    CHECK_EQ(smp[2], PIOENC_SMP_BEAT1);
    // streams that disagree on length, or a BEAT1 without a sampled cycle, are rejected
    CHECK(encode_all(s4, 4));
    CHECK_EQ(pioenc_decode(cmd, nc, wr, nw, smp, ns, back, 8192), 4);
    CHECK_EQ(pioenc_decode(cmd, nc, wr, nw, smp, ns - 1, back, 8192), 0);
    uint32_t bad = PIOENC_SMP_BEAT1 | 3u << 2;
    CHECK_EQ(pioenc_decode(cmd, nc, wr, nw, &bad, 1, back, 8192), 0);
    uint32_t kind3 = 3u | 3u << 2;
    CHECK_EQ(pioenc_decode(cmd, nc, &kind3, 1, smp, ns, back, 8192), 0);
}

static void test_timing(void)
{
    pioenc_timing_t t;
    // 2 us at 150 MHz: 75 system clocks per quarter = div x (1 + delay), exactly
    CHECK(pioenc_timing(2000, 150000000, 1, 7, &t));
    CHECK_EQ(t.tck_ps, 2000000);
    CHECK_EQ(t.div * (t.delay + 1), 75);
    // 1 us: 37.5 per quarter can't be exact; within one system clock per quarter
    CHECK(pioenc_timing(1000, 150000000, 1, 7, &t));
    CHECK(t.tck_ps >= 986000 && t.tck_ps <= 1014000);
    // two instructions per quarter: 4 us is 150 per quarter = 2 x div x (1 + delay)
    CHECK(pioenc_timing(4000, 150000000, 2, 7, &t));
    CHECK_EQ(t.tck_ps, 4000000);
    CHECK_EQ(2 * t.div * (t.delay + 1), 150);
    // 250 ns works; 8 ns (the DLL-off minimum) is beyond a 4-quarter PIO loop
    CHECK(pioenc_timing(250, 150000000, 1, 7, &t));
    CHECK(!pioenc_timing(8, 150000000, 1, 7, &t));
}

void test_pio_enc(void)
{
    RUN(test_real_sequences_round_trip);
    RUN(test_runs_split_at_the_repeat_limit);
    RUN(test_layout_and_disagreement);
    RUN(test_timing);
}
