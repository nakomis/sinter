// The PIO executor's word streams (src/pio_enc.c): every sequence the
// firmware builds survives encode/decode exactly, compresses its idle
// stretches, and still meets JEDEC when played back from the decoded form.

#include <string.h>

#include "check.h"
#include "ddr3.h"
#include "jedec.h"
#include "pio_enc.h"

static ddr3_cycle_t seq_cyc[8192], back[8192];
static uint32_t cmd[8192], data[8192];

static bool round_trips(const ddr3_seq_t *s, size_t *words)
{
    bool ovf;
    size_t w = pioenc_encode(s->cycles, s->len, cmd, data, 8192, &ovf);
    if (ovf) return false;
    size_t n = pioenc_decode(cmd, data, w, back, 8192);
    if (n != s->len) return false;
    for (size_t i = 0; i < n; i++) {
        ddr3_cycle_t a = pioenc_normalise(s->cycles[i]), b = back[i];
        if (a.ca != b.ca || a.flags != b.flags || a.dq[0] != b.dq[0] || a.dq[1] != b.dq[1])
            return false;
    }
    if (words) *words = w;
    return true;
}

static void test_real_sequences_round_trip(void)
{
    static uint8_t d[8 * 128];
    for (size_t i = 0; i < sizeof d; i++) d[i] = (uint8_t)(i * 13 + 1);
    const uint32_t tcks[] = { 250, 1000, 3400 };
    for (int k = 0; k < 3; k++) {
        ddr3_seq_t s;
        size_t w;
        // init: 512 CK of tZQinit NOPs collapse to a word
        ddr3_seq_init(&s, seq_cyc, 8192, tcks[k]);
        ddr3_seq_init_mrs(&s);
        ddr3_seq_sre(&s);
        CHECK(round_trips(&s, &w));
        CHECK(w * 4 < s.len);
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
            CHECK(round_trips(&s, &w));
            CHECK(w < s.len);
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
    size_t w = pioenc_encode(seq_cyc, 2000, cmd, data, 8192, &ovf);
    CHECK_EQ(w, 4);                                   // 512 + 512 + 512 + 464
    CHECK_EQ(cmd[0] >> PIOENC_REP_SHIFT, 511);
    CHECK_EQ(cmd[3] >> PIOENC_REP_SHIFT, 463);
    CHECK_EQ(pioenc_decode(cmd, data, w, back, 8192), 2000);
    CHECK_EQ(pioenc_encode(seq_cyc, 2000, cmd, data, 2, &ovf), 2);
    CHECK(ovf);
}

static void test_layout_and_disagreement(void)
{
    ddr3_cycle_t c = { .ca = ddr3_encode(DDR3_ACT, 5, 0x2ABC, true) | (1u << CA_CK),
                       .flags = CY_DRIVE | CY_STROBE, .dq = { 0xA5, 0x3C } };
    bool ovf;
    CHECK_EQ(pioenc_encode(&c, 1, cmd, data, 4, &ovf), 1);
    CHECK_EQ(cmd[0], (c.ca >> CA_CKE) & 0x7FFFFFu);   // CK dropped, C/A from GPIO 18
    CHECK_EQ(data[0], 3u | 0xA5u << 4 | 0x3Cu << 12);
    // a sampled cycle marks the next one, which splits a run of sampling once
    ddr3_cycle_t smp[4] = { { .flags = CY_SAMPLE }, { .flags = CY_SAMPLE }, { .flags = CY_SAMPLE }, { 0 } };
    CHECK_EQ(pioenc_encode(smp, 4, cmd, data, 8, &ovf), 3);
    CHECK_EQ(data[0] & 0xF, CY_SAMPLE);
    CHECK_EQ(data[1] & 0xF, CY_SAMPLE | PIOENC_PREV_SAMPLE);
    CHECK_EQ(data[1] >> PIOENC_REP_SHIFT, 1);
    CHECK_EQ(data[2] & 0xF, PIOENC_PREV_SAMPLE);
    // write data that isn't driven doesn't split runs
    ddr3_cycle_t idle[2] = { { .ca = 0, .dq = { 1, 2 } }, { .ca = 0, .dq = { 3, 4 } } };
    CHECK_EQ(pioenc_encode(idle, 2, cmd, data, 4, &ovf), 1);
    // streams that disagree on a repeat count are rejected
    cmd[0] += 1u << PIOENC_REP_SHIFT;
    CHECK_EQ(pioenc_decode(cmd, data, 1, back, 8192), 0);
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
