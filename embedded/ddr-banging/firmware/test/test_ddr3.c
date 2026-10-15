#include <string.h>

#include "check.h"
#include "ddr3.h"

#define BIT(word, b) (((word) >> (b)) & 1u)

static unsigned ras(uint32_t w) { return BIT(w, CA_RASN); }
static unsigned cas(uint32_t w) { return BIT(w, CA_CASN); }
static unsigned we(uint32_t w)  { return BIT(w, CA_WEN); }
static unsigned cs0(uint32_t w) { return BIT(w, CA_CS0N); }
static unsigned ba(uint32_t w)  { return (w >> CA_BA0) & 7u; }
static unsigned a(uint32_t w)   { return (w >> CA_A0) & 0x3FFFu; }

// Classify a CA word back into a command (inverse of ddr3_encode).
static ddr3_cmd_t decode(uint32_t w)
{
    if (cs0(w)) return DDR3_DES;
    unsigned k = ras(w) << 2 | cas(w) << 1 | we(w);
    switch (k) {
    case 0: return DDR3_MRS;
    case 1: return DDR3_REF;
    case 2: return DDR3_PRE;
    case 3: return DDR3_ACT;
    case 4: return DDR3_WR;
    case 5: return DDR3_RD;
    case 6: return DDR3_ZQCL;
    default: return DDR3_NOP;
    }
}

static ddr3_cycle_t buf[4096];

static long find(const ddr3_seq_t *s, ddr3_cmd_t cmd, size_t from)
{
    for (size_t i = from; i < s->len; i++)
        if (decode(s->cycles[i].ca) == cmd) return (long)i;
    return -1;
}

static int count(const ddr3_seq_t *s, ddr3_cmd_t cmd)
{
    int n = 0;
    for (size_t i = 0; i < s->len; i++) n += decode(s->cycles[i].ca) == cmd;
    return n;
}

// Longest run of cycles between REFs (or from the start/end) with CKE high.
static size_t max_ref_gap(const ddr3_seq_t *s, size_t from)
{
    size_t gap = 0, best = 0;
    for (size_t i = from; i < s->len; i++) {
        if (decode(s->cycles[i].ca) == DDR3_REF) gap = 0;
        else if (++gap > best) best = gap;
    }
    return best;
}

static void test_encode(void)
{
    uint32_t nop = ddr3_encode(DDR3_NOP, 0, 0, true);
    CHECK_EQ(BIT(nop, CA_CKE), 1);
    CHECK_EQ(BIT(nop, CA_CS0N), 0);
    CHECK_EQ(BIT(nop, CA_CS1N), 1);
    CHECK_EQ(ras(nop) << 2 | cas(nop) << 1 | we(nop), 7);
    CHECK_EQ(nop & ((1u << CA_CK) | (1u << CA_CKN)), 0);

    CHECK_EQ(BIT(ddr3_encode(DDR3_NOP, 0, 0, false), CA_CKE), 0);
    CHECK_EQ(cs0(ddr3_encode(DDR3_DES, 0, 0, true)), 1);
    CHECK_EQ(BIT(ddr3_encode(DDR3_DES, 0, 0, true), CA_CS1N), 1);

    uint32_t act = ddr3_encode(DDR3_ACT, 5, 0x1234, true);
    CHECK_EQ(decode(act), DDR3_ACT);
    CHECK_EQ(ba(act), 5);
    CHECK_EQ(a(act), 0x1234);

    ddr3_cmd_t all[] = { DDR3_MRS, DDR3_REF, DDR3_PRE, DDR3_ACT, DDR3_WR, DDR3_RD, DDR3_ZQCL };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        CHECK_EQ(decode(ddr3_encode(all[i], 0, 0, true)), all[i]);

    // ZQCL needs A10 high (ZQCS has it low)
    CHECK_EQ(BIT(a(ddr3_encode(DDR3_ZQCL, 0, 0, true)), 10), 1);
    // address and bank fields are masked to their widths
    CHECK_EQ(a(ddr3_encode(DDR3_ACT, 0xF, 0xFFFFF, true)), 0x3FFF);
    CHECK_EQ(ba(ddr3_encode(DDR3_ACT, 0xF, 0, true)), 7);
    CHECK_EQ(ddr3_encode(DDR3_ACT, 7, 0x3FFF, true) & ~CA_MASK, 0);
}

static void test_mode_registers(void)
{
    CHECK_EQ(ddr3_mr0(), 0x0420);  // WR=6 (A11:9=010), CL6 (A6:4=010, A2=0), BL8
    CHECK_EQ(ddr3_mr1(), 0x0001);  // DLL disable
    CHECK_EQ(ddr3_mr2(), 0x0008);  // CWL6 (A5:3=001)
    CHECK_EQ(ddr3_mr3(), 0x0000);
    CHECK_EQ(ddr3_mr0() & (1u << 8), 0);  // no DLL reset
}

static void test_split(void)
{
    ddr3_addr_t x = ddr3_split(0);
    CHECK_EQ(x.bank, 0); CHECK_EQ(x.row, 0); CHECK_EQ(x.col, 0);
    x = ddr3_split(0x3F8);
    CHECK_EQ(x.col, 0x3F8); CHECK_EQ(x.bank, 0);
    x = ddr3_split(0x400);
    CHECK_EQ(x.col, 0); CHECK_EQ(x.bank, 1);
    x = ddr3_split(DDR3_CHIP_BYTES - 8);
    CHECK_EQ(x.row, 0x3FFF); CHECK_EQ(x.bank, 7); CHECK_EQ(x.col, 0x3F8);
    CHECK_EQ(DDR3_CHIP_BYTES, 128u * 1024 * 1024);
}

static void test_clock_counts(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    CHECK_EQ(ddr3_ck(&s, 15, 4), 4);
    CHECK_EQ(ddr3_ck(&s, 14, 1), 1);
    CHECK_EQ(ddr3_ck(&s, 2000, 1), 2);
    CHECK_EQ(ddr3_ck(&s, 2001, 1), 3);
    CHECK_EQ(s.ref_interval, 7);  // 7.8 us / 1 us, rounded down
    ddr3_seq_init(&s, buf, 4096, 5000);
    CHECK_EQ(s.ref_interval, 1);
}

static void test_idle_refreshes_when_banks_closed(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    ddr3_seq_idle(&s, 100);
    CHECK_EQ(s.len, 100);
    CHECK(count(&s, DDR3_REF) >= 100 / 7);
    CHECK(max_ref_gap(&s, 0) < 7);
    CHECK(!s.overflow);
}

static void test_idle_never_refreshes_with_open_bank(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    ddr3_seq_cmd(&s, DDR3_ACT, 2, 0x10);
    ddr3_seq_idle(&s, 20);
    CHECK_EQ(count(&s, DDR3_REF), 0);
}

static void test_postponed_refreshes_are_caught_up(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    ddr3_seq_cmd(&s, DDR3_ACT, 0, 0);
    ddr3_seq_idle(&s, 40);              // ~5 tREFI with a row open: 5 REFs owed
    ddr3_seq_cmd(&s, DDR3_PRE, 0, 1u << 10);
    size_t from = s.len;
    ddr3_seq_idle(&s, 12);
    int refs = 0;
    for (size_t i = from; i < s.len; i++) refs += decode(s.cycles[i].ca) == DDR3_REF;
    CHECK(refs >= 5);
}

static void test_refs_back_to_back_when_trfc_fits_a_clock(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);  // tRFC 110 ns < 1 CK
    s.cke = true;
    s.since_ref = 50;                    // 7 REFs owed
    ddr3_seq_idle(&s, 10);
    long first = find(&s, DDR3_REF, 0);
    CHECK_EQ(find(&s, DDR3_REF, (size_t)first + 1), first + 1);
}

static void test_refs_respect_trfc_at_fast_clock(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 8);    // 125 MHz: tRFC = 14 CK, tREFI = 975 CK
    s.cke = true;
    s.since_ref = 5000;                 // plenty owed
    ddr3_seq_idle(&s, 200);
    CHECK(count(&s, DDR3_REF) >= 5);
    long last = -100;
    for (size_t i = 0; i < s.len; i++) {
        if (decode(s.cycles[i].ca) != DDR3_REF) continue;
        CHECK((long)i - last >= 14);
        CHECK(s.len - i > 14);          // its tRFC fits inside the idle window
        last = (long)i;
    }
}

// Postponed REFs at any point: owed = elapsed / tREFI - REFs issued.
static long worst_postponed(const ddr3_seq_t *s, size_t from, long owed0)
{
    long refs = 0, worst = 0;
    double per = 7800.0 / s->tck_ns;
    for (size_t i = from; i < s->len; i++) {
        if (decode(s->cycles[i].ca) == DDR3_REF) refs++;
        long owed = owed0 + (long)((i - from + 1) / per) - refs;
        if (owed > worst) worst = owed;
    }
    return worst;
}

static void test_refresh_keeps_pace_up_to_3900ns(void)
{
    const uint32_t tcks[] = { 250, 1000, 2000, 3000, 3900 };
    for (int k = 0; k < 5; k++) {
        ddr3_seq_t s;
        ddr3_seq_init(&s, buf, 4096, tcks[k]);
        s.cke = true;
        ddr3_seq_idle(&s, 2000);
        CHECK(worst_postponed(&s, 0, 0) <= 1);
        // a write keeps its row open for less than tRAS(max) = 9 x tREFI
        ddr3_seq_init(&s, buf, 4096, tcks[k]);
        s.cke = true;
        const uint8_t d[8] = { 0 };
        ddr3_seq_write(&s, 0, d);
        long act = find(&s, DDR3_ACT, 0), pre = find(&s, DDR3_PRE, 0);
        CHECK((pre - act) * (long)tcks[k] < 9 * 7800);
    }
}

static void test_command_session_stays_within_8_postponed(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    const uint8_t d[8] = { 0 };
    for (int k = 0; k < 4; k++) {   // four shell commands back to back
        ddr3_seq_srx(&s);
        if (k % 2) ddr3_seq_read(&s, 0x40);
        else ddr3_seq_write(&s, 0x40, d);
        size_t before_sre = s.len;
        ddr3_seq_sre(&s);
        CHECK(s.since_ref < s.ref_interval);   // debt paid before self-refresh
        (void)before_sre;
    }
    CHECK(!s.overflow);
}

// What dram.c builds for a 1 KB transfer: one self-refresh exit, 128 bursts
// each followed by a refresh catch-up, then self-refresh entry.
static void test_batched_bursts_keep_refresh_pace(void)
{
    static ddr3_cycle_t big[8192];
    const uint32_t tcks[] = { 250, 1000, 2000, 3000, 3900 };
    const uint8_t d[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (int k = 0; k < 5; k++)
        for (int rd = 0; rd < 2; rd++) {
            ddr3_seq_t s;
            ddr3_seq_init(&s, big, 8192, tcks[k]);
            ddr3_seq_srx(&s);
            size_t from = s.len;
            for (int j = 0; j < 128; j++) {
                if (rd) ddr3_seq_read(&s, 8u * j);
                else ddr3_seq_write(&s, 8u * j, d);
                ddr3_seq_catch_up(&s);
                CHECK(s.since_ref < s.ref_interval);
            }
            ddr3_seq_sre(&s);
            CHECK(!s.overflow);
            CHECK(worst_postponed(&s, from, 0) <= 8);
        }
}

static void test_self_refresh_entry_converges_when_slow(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 3900);   // tREFI is 2 CK
    s.cke = true;
    s.since_ref = 8 * s.ref_interval;    // the most a command may leave owed
    ddr3_seq_sre(&s);
    CHECK(!s.overflow);
    int refs = 0;   // the 8 owed, then the SRE (a REF with CKE low)
    for (size_t i = 0; i < s.len; i++) refs += decode(s.cycles[i].ca) == DDR3_REF;
    CHECK(refs >= 9);
}

static void test_init_mrs(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    ddr3_seq_init_mrs(&s);
    CHECK(!s.overflow);

    // clock runs >= 5 cycles with CKE low before CKE goes high
    size_t i = 0;
    while (i < s.len && !BIT(s.cycles[i].ca, CA_CKE)) i++;
    CHECK(i >= 5);
    // only NOP/DES until tXPR (>= 5 CK)
    long mr = find(&s, DDR3_MRS, 0);
    CHECK(mr - (long)i >= 5);

    unsigned order[4] = { 2, 3, 1, 0 };
    uint16_t val[4] = { ddr3_mr2(), ddr3_mr3(), ddr3_mr1(), ddr3_mr0() };
    long prev = -100;
    for (int k = 0; k < 4; k++) {
        CHECK(mr >= 0);
        if (mr < 0) return;
        CHECK_EQ(ba(s.cycles[mr].ca), order[k]);
        CHECK_EQ(a(s.cycles[mr].ca), val[k]);
        CHECK(mr - prev >= 4);  // tMRD
        prev = mr;
        mr = find(&s, DDR3_MRS, (size_t)mr + 1);
    }
    CHECK_EQ(count(&s, DDR3_MRS), 4);
    long zq = find(&s, DDR3_ZQCL, 0);
    CHECK(zq - prev >= 12);  // tMOD
    CHECK((long)s.len - zq >= 512);  // tZQinit
    // nothing but NOPs during calibration (JEDEC: no other activity for tZQinit)
    for (size_t k = (size_t)zq + 1; k < (size_t)zq + 512 && k < s.len; k++)
        CHECK_EQ(decode(s.cycles[k].ca), DDR3_NOP);
    CHECK(s.cke);
}

static void test_write_burst(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    const uint8_t d[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint32_t addr = (0x123u << 13) | (5u << 10) | 0x48;
    ddr3_seq_write(&s, addr, d);
    CHECK(!s.overflow);

    long act = find(&s, DDR3_ACT, 0);
    long wr = find(&s, DDR3_WR, 0);
    long pre = find(&s, DDR3_PRE, 0);
    CHECK(act >= 0 && wr > act && pre > wr);
    if (act < 0 || wr < 0 || pre < 0) return;
    CHECK_EQ(ba(s.cycles[act].ca), 5);
    CHECK_EQ(a(s.cycles[act].ca), 0x123);
    CHECK_EQ(ba(s.cycles[wr].ca), 5);
    CHECK_EQ(a(s.cycles[wr].ca), 0x48);  // A10 = 0: no auto-precharge, A12 = 0: BC4 off (BL8 fixed anyway)
    CHECK(wr - act >= 1);  // tRCD

    // preamble, four strobed cycles of data, postamble
    size_t w = (size_t)wr + DDR3_CWL;
    CHECK_EQ(s.cycles[w - 1].flags, CY_DRIVE);
    for (int k = 0; k < 4; k++) {
        CHECK_EQ(s.cycles[w + k].flags, CY_DRIVE | CY_STROBE);
        CHECK_EQ(s.cycles[w + k].dq[0], d[2 * k]);
        CHECK_EQ(s.cycles[w + k].dq[1], d[2 * k + 1]);
    }
    CHECK_EQ(s.cycles[w + 4].flags, CY_DRIVE);
    // nothing else drives
    int driven = 0;
    for (size_t i = 0; i < s.len; i++) driven += (s.cycles[i].flags & CY_DRIVE) != 0;
    CHECK_EQ(driven, 6);
    // PRE waits for tWR (>= 15 ns -> 1 CK, use 4) after the last data
    CHECK((size_t)pre >= w + 4 + 4);
    for (long k = act; k < pre; k++)  // never with a row open
        CHECK(decode(s.cycles[k].ca) != DDR3_REF);
    CHECK_EQ(s.open_banks, 0);
}

static void test_read_burst(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    ddr3_seq_read(&s, (7u << 13) | (3u << 10) | 0x10);
    CHECK(!s.overflow);
    long rd = find(&s, DDR3_RD, 0);
    CHECK(rd > 0);
    if (rd < 0) return;
    CHECK_EQ(ba(s.cycles[rd].ca), 3);
    CHECK_EQ(a(s.cycles[rd].ca), 0x10);
    size_t first = (size_t)rd + DDR3_RL_DLL_OFF - DDR3_READ_WINDOW_BEFORE;
    CHECK_EQ(ddr3_seq_last_read_start(&s), first);
    for (size_t i = 0; i < s.len; i++) {
        bool in = i >= first && i < first + DDR3_READ_WINDOW_CYCLES;
        CHECK_EQ((s.cycles[i].flags & CY_SAMPLE) != 0, in);
        CHECK_EQ(s.cycles[i].flags & CY_DRIVE, 0);
    }
    // the sequence ends after the window, so its last sample point exists
    CHECK(s.len > first + DDR3_READ_WINDOW_CYCLES);
    long pre = find(&s, DDR3_PRE, 0);
    CHECK(pre - rd >= 4);  // tRTP
    CHECK_EQ(s.open_banks, 0);
}

static void test_self_refresh(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 4096, 1000);
    s.cke = true;
    ddr3_seq_sre(&s);
    CHECK(!s.cke);
    long pre = find(&s, DDR3_PRE, 0);
    long ref = find(&s, DDR3_REF, 0);
    CHECK(pre >= 0 && ref > pre);
    if (ref < 0) return;
    CHECK_EQ(BIT(a(s.cycles[pre].ca), 10), 1);       // precharge all
    CHECK_EQ(BIT(s.cycles[ref].ca, CA_CKE), 0);      // REF with CKE low = SRE
    CHECK((long)s.len - ref >= 5);                   // clock runs tCKSRE after
    for (size_t i = (size_t)ref; i < s.len; i++) CHECK_EQ(BIT(s.cycles[i].ca, CA_CKE), 0);

    size_t start = s.len;
    ddr3_seq_srx(&s);
    CHECK(s.cke);
    size_t i = start;
    while (i < s.len && !BIT(s.cycles[i].ca, CA_CKE)) i++;
    CHECK(i - start >= 5);                           // tCKSRX
    CHECK_EQ(decode(s.cycles[i].ca), DDR3_NOP);      // CKE rises with NOP
    CHECK(s.len - i >= 512);                         // tXSDLL
    CHECK(max_ref_gap(&s, i) < 7);
}

static void test_overflow_is_flagged(void)
{
    ddr3_seq_t s;
    ddr3_seq_init(&s, buf, 10, 1000);
    s.cke = true;
    ddr3_seq_idle(&s, 20);
    CHECK(s.overflow);
    CHECK_EQ(s.len, 10);
}

void test_ddr3(void)
{
    RUN(test_encode);
    RUN(test_mode_registers);
    RUN(test_split);
    RUN(test_clock_counts);
    RUN(test_idle_refreshes_when_banks_closed);
    RUN(test_idle_never_refreshes_with_open_bank);
    RUN(test_postponed_refreshes_are_caught_up);
    RUN(test_refs_back_to_back_when_trfc_fits_a_clock);
    RUN(test_refs_respect_trfc_at_fast_clock);
    RUN(test_refresh_keeps_pace_up_to_3900ns);
    RUN(test_command_session_stays_within_8_postponed);
    RUN(test_batched_bursts_keep_refresh_pace);
    RUN(test_self_refresh_entry_converges_when_slow);
    RUN(test_init_mrs);
    RUN(test_write_burst);
    RUN(test_read_burst);
    RUN(test_self_refresh);
    RUN(test_overflow_is_flagged);
}
