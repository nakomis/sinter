#include "ddr3.h"

#define TREFI_NS  7800u
#define TRFC_NS   110u   // 1 Gb
#define TRCD_NS   15u
#define TRP_NS    15u
#define TRAS_NS   38u
#define TWR_NS    15u

uint32_t ddr3_encode(ddr3_cmd_t cmd, unsigned ba, unsigned addr, bool cke)
{
    // CS#, RAS#, CAS#, WE# per the JESD79-3 command truth table
    unsigned cs = 0, r = 1, c = 1, w = 1;
    switch (cmd) {
    case DDR3_DES:  cs = 1; break;
    case DDR3_NOP:  break;
    case DDR3_MRS:  r = 0; c = 0; w = 0; break;
    case DDR3_REF:  r = 0; c = 0; break;
    case DDR3_PRE:  r = 0; w = 0; break;
    case DDR3_ACT:  r = 0; break;
    case DDR3_WR:   c = 0; w = 0; break;
    case DDR3_RD:   c = 0; break;
    case DDR3_ZQCL: w = 0; addr |= 1u << 10; break;
    }
    return (uint32_t)cke << CA_CKE
         | cs << CA_CS0N
         | 1u << CA_CS1N
         | r << CA_RASN
         | c << CA_CASN
         | w << CA_WEN
         | (ba & 7u) << CA_BA0
         | (addr & 0x3FFFu) << CA_A0;
}

uint16_t ddr3_mr0(void)
{
    return 2u << 9     // WR = 6 (only used by auto-precharge)
         | 2u << 4;    // CL = 6 (A6:A4 = 010, A2 = 0); BL8 fixed, sequential
}

uint16_t ddr3_mr1(void)
{
    return 1u;         // DLL disable; RZQ/6 drive, RTT_NOM off, AL = 0, outputs on
}

uint16_t ddr3_mr2(void)
{
    return 1u << 3;    // CWL = 6; RTT_WR off, normal self-refresh range
}

uint16_t ddr3_mr3(void)
{
    return 0;
}

ddr3_addr_t ddr3_split(uint32_t byte_addr)
{
    ddr3_addr_t x;
    x.col  = byte_addr & ((1u << DDR3_COL_BITS) - 1);
    x.bank = (byte_addr >> DDR3_COL_BITS) & ((1u << DDR3_BANK_BITS) - 1);
    x.row  = (byte_addr >> (DDR3_COL_BITS + DDR3_BANK_BITS)) & ((1u << DDR3_ROW_BITS) - 1);
    return x;
}

void ddr3_seq_init(ddr3_seq_t *s, ddr3_cycle_t *buf, size_t cap, uint32_t tck_ns)
{
    s->cycles = buf;
    s->cap = cap;
    s->len = 0;
    s->overflow = false;
    s->cke = false;
    s->tck_ns = tck_ns;
    s->since_ref = 0;
    s->ref_interval = TREFI_NS / tck_ns ? TREFI_NS / tck_ns : 1;
    s->open_banks = 0;
    s->ref_hold = 0;
    s->last_read = 0;
}

uint32_t ddr3_ck(const ddr3_seq_t *s, uint32_t ns, uint32_t min_ck)
{
    uint32_t n = (ns + s->tck_ns - 1) / s->tck_ns;
    return n > min_ck ? n : min_ck;
}

static ddr3_cycle_t *push(ddr3_seq_t *s, uint32_t ca)
{
    if (s->len >= s->cap) {
        s->overflow = true;
        return NULL;
    }
    ddr3_cycle_t *c = &s->cycles[s->len++];
    c->ca = ca;
    c->flags = 0;
    c->dq[0] = c->dq[1] = 0;
    s->since_ref++;
    if (s->ref_hold) s->ref_hold--;
    return c;
}

void ddr3_seq_cmd(ddr3_seq_t *s, ddr3_cmd_t cmd, unsigned ba, unsigned addr)
{
    push(s, ddr3_encode(cmd, ba, addr, s->cke));
    if (cmd == DDR3_REF) {
        // pay off one tREFI of refresh debt; postponed REFs stay owed
        s->since_ref = s->since_ref > s->ref_interval ? s->since_ref - s->ref_interval : 0;
        s->ref_hold = ddr3_ck(s, TRFC_NS, 1) - 1;   // cycles still inside tRFC
    }
    if (cmd == DDR3_ACT) s->open_banks |= 1u << (ba & 7u);
    if (cmd == DDR3_PRE) s->open_banks = (addr & (1u << 10)) ? 0 : s->open_banks & ~(1u << (ba & 7u));
}

// n NOP cycles, no refresh: for windows where only NOP is allowed.
static void nops(ddr3_seq_t *s, uint32_t n)
{
    while (n--) ddr3_seq_cmd(s, DDR3_NOP, 0, 0);
}

void ddr3_seq_idle(ddr3_seq_t *s, uint32_t n)
{
    uint32_t trfc = ddr3_ck(s, TRFC_NS, 1);
    while (n) {
        // REF when one is due (leaving room for its tRFC before the next is
        // due), only with every bank closed, tRFC clear of the previous REF,
        // and room for this REF's tRFC before the idle window ends
        bool due = s->since_ref + trfc >= s->ref_interval;
        if (s->cke && !s->open_banks && due && !s->ref_hold && n > trfc)
            ddr3_seq_cmd(s, DDR3_REF, 0, 0);
        else
            ddr3_seq_cmd(s, DDR3_NOP, 0, 0);
        n--;
    }
}

void ddr3_seq_init_mrs(ddr3_seq_t *s)
{
    // Clock valid for >= 5 cycles with CKE low, then CKE high
    s->cke = false;
    nops(s, 5);
    s->cke = true;
    nops(s, ddr3_ck(s, TRFC_NS + 10, 5));       // tXPR

    const uint16_t mr[4] = { ddr3_mr2(), ddr3_mr3(), ddr3_mr1(), ddr3_mr0() };
    const unsigned which[4] = { 2, 3, 1, 0 };
    for (int k = 0; k < 4; k++) {
        ddr3_seq_cmd(s, DDR3_MRS, which[k], mr[k]);
        nops(s, k < 3 ? 3 : ddr3_ck(s, 15, 12) - 1);   // tMRD = 4, then tMOD
    }
    ddr3_seq_cmd(s, DDR3_ZQCL, 0, 0);
    nops(s, 512);                               // tZQinit: NOP only
    s->since_ref = s->ref_interval;             // refresh straight away
}

static void open_row(ddr3_seq_t *s, ddr3_addr_t x)
{
    // no REF can be issued while the row is open, and at tCK 3.4 us a burst
    // keeps it open for ~7 tREFI: start with nothing owed (JEDEC: <= 8 postponed)
    if (!s->open_banks) ddr3_seq_catch_up(s);
    ddr3_seq_cmd(s, DDR3_ACT, x.bank, x.row);
    nops(s, ddr3_ck(s, TRCD_NS, 1) - 1);
}

static void close_row(ddr3_seq_t *s, ddr3_addr_t x)
{
    ddr3_seq_cmd(s, DDR3_PRE, x.bank, 0);
    ddr3_seq_idle(s, ddr3_ck(s, TRP_NS, 1));
}

#define TCCD 4   // CK between column commands; = BL8 / 2, so data is seamless

// Append n NOPs and return the first, for laying out overlapping commands
// and data by position. NULL on overflow.
static ddr3_cycle_t *span(ddr3_seq_t *s, uint32_t n)
{
    size_t at = s->len;
    nops(s, n);
    return s->overflow ? NULL : &s->cycles[at];
}

static void put_cmd(ddr3_seq_t *s, ddr3_cycle_t *c, ddr3_cmd_t cmd, unsigned ba, unsigned addr)
{
    c->ca = ddr3_encode(cmd, ba, addr, s->cke);
}

static uint32_t twr_ck(const ddr3_seq_t *s)
{
    return ddr3_ck(s, TWR_NS, 4);   // DLL off: the greater of 4 CK and 15 ns (Micron)
}

unsigned ddr3_seq_row_bursts(const ddr3_seq_t *s)
{
    // ACT .. last WR = tRCD + 4(n-1); then WL, 4 data, postamble, tWR, PRE,
    // tRP, REF. One CK spare for where the REF schedule's phase falls.
    long fixed = (long)ddr3_ck(s, TRCD_NS, 1) + DDR3_CWL + 5 + (long)twr_ck(s) - 1
               + 1 + (long)ddr3_ck(s, TRP_NS, 1) + 1;
    long room = 8L * (long)s->ref_interval - 1 - fixed;
    long n = room < 0 ? 1 : room / TCCD + 1;
    long row = 1L << (DDR3_COL_BITS - 3);
    return (unsigned)(n > row ? row : n);
}

// A run stops at the end of its row: a column past 1023 would spill into A10
// (auto-precharge).
static unsigned clamp_to_row(ddr3_addr_t x, unsigned n)
{
    unsigned left = ((1u << DDR3_COL_BITS) - x.col) / 8;
    return n < left ? n : left;
}

unsigned ddr3_seq_write_run(ddr3_seq_t *s, uint32_t byte_addr, const uint8_t *data, unsigned n)
{
    ddr3_addr_t x = ddr3_split(byte_addr & ~7u);
    n = clamp_to_row(x, n);
    open_row(s, x);
    // WR j at 4j; data for WR j at 4j + WL .. +3, preamble before the first,
    // postamble after the last
    uint32_t len = TCCD * (n - 1) + DDR3_CWL + 5;
    ddr3_cycle_t *c = span(s, len);
    if (!c) return n;
    for (unsigned j = 0; j < n; j++) put_cmd(s, &c[TCCD * j], DDR3_WR, x.bank, x.col + 8 * j);
    c[DDR3_CWL - 1].flags = CY_DRIVE;
    for (unsigned j = 0; j < n; j++)
        for (int k = 0; k < 4; k++) {
            ddr3_cycle_t *d = &c[TCCD * j + DDR3_CWL + k];
            d->flags = CY_DRIVE | CY_STROBE;
            d->dq[0] = data[8 * j + 2 * k];
            d->dq[1] = data[8 * j + 2 * k + 1];
        }
    c[len - 1].flags = CY_DRIVE;
    nops(s, twr_ck(s) - 1);                  // tWR
    close_row(s, x);
    return n;
}

unsigned ddr3_read_run_samples(unsigned n)
{
    return DDR3_READ_WINDOW_CYCLES + TCCD * (n - 1);
}

unsigned ddr3_seq_read_run(ddr3_seq_t *s, uint32_t byte_addr, unsigned n)
{
    ddr3_addr_t x = ddr3_split(byte_addr & ~7u);
    n = clamp_to_row(x, n);
    open_row(s, x);
    uint32_t w0 = DDR3_RL_DLL_OFF - DDR3_READ_WINDOW_BEFORE;   // first sampled cycle
    uint32_t ns = ddr3_read_run_samples(n);
    // the window ends past tRTP (4 CK) and tRAS; one more cycle carries the
    // last falling-edge sample point before the PRE
    ddr3_cycle_t *c = span(s, w0 + ns + 1);
    if (!c) return n;
    for (unsigned j = 0; j < n; j++) put_cmd(s, &c[TCCD * j], DDR3_RD, x.bank, x.col + 8 * j);
    for (uint32_t k = 0; k < ns; k++) c[w0 + k].flags = CY_SAMPLE;
    s->last_read = (size_t)(c - s->cycles) + w0;
    close_row(s, x);
    return n;
}

void ddr3_seq_write(ddr3_seq_t *s, uint32_t byte_addr, const uint8_t data[8])
{
    ddr3_seq_write_run(s, byte_addr, data, 1);
}

void ddr3_seq_read(ddr3_seq_t *s, uint32_t byte_addr)
{
    ddr3_seq_read_run(s, byte_addr, 1);
}

size_t ddr3_seq_last_read_start(const ddr3_seq_t *s)
{
    return s->last_read;
}

void ddr3_seq_catch_up(ddr3_seq_t *s)
{
    // REFs back to back as tRFC allows. Each REF cycle pays a whole tREFI
    // and costs one CK (plus tRFC), so this converges even when tREFI is
    // only 2 CK; idling for tRFC + 1 per REF would not.
    while (s->since_ref >= s->ref_interval && !s->overflow)
        ddr3_seq_cmd(s, s->ref_hold ? DDR3_NOP : DDR3_REF, 0, 0);
    nops(s, s->ref_hold);
}

void ddr3_seq_sre(ddr3_seq_t *s)
{
    ddr3_seq_cmd(s, DDR3_PRE, 0, 1u << 10);
    nops(s, ddr3_ck(s, TRP_NS, 1));
    ddr3_seq_catch_up(s);   // postponed REFs: JEDEC counts them across self-refresh
    s->cke = false;
    ddr3_seq_cmd(s, DDR3_REF, 0, 0);            // REF with CKE low = SRE
    nops(s, 5);                                 // tCKSRE / tCKESR with clock running
}

void ddr3_seq_srx(ddr3_seq_t *s)
{
    s->cke = false;
    nops(s, 5);                                 // tCKSRX: clock stable first
    s->cke = true;
    s->since_ref = 0;
    nops(s, ddr3_ck(s, TRFC_NS + 10, 5));       // tXS
    ddr3_seq_idle(s, 512);                      // tXSDLL, refreshing
}
