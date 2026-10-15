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
    ddr3_seq_cmd(s, DDR3_ACT, x.bank, x.row);
    nops(s, ddr3_ck(s, TRCD_NS, 1) - 1);
}

static void close_row(ddr3_seq_t *s, ddr3_addr_t x)
{
    ddr3_seq_cmd(s, DDR3_PRE, x.bank, 0);
    ddr3_seq_idle(s, ddr3_ck(s, TRP_NS, 1));
}

void ddr3_seq_write(ddr3_seq_t *s, uint32_t byte_addr, const uint8_t data[8])
{
    ddr3_addr_t x = ddr3_split(byte_addr & ~7u);
    open_row(s, x);
    ddr3_seq_cmd(s, DDR3_WR, x.bank, x.col);
    nops(s, DDR3_CWL - 2);
    ddr3_cycle_t *c = push(s, ddr3_encode(DDR3_NOP, 0, 0, s->cke));
    if (c) c->flags = CY_DRIVE;                 // preamble: DQS driven low
    for (int k = 0; k < 4; k++) {
        c = push(s, ddr3_encode(DDR3_NOP, 0, 0, s->cke));
        if (!c) return;
        c->flags = CY_DRIVE | CY_STROBE;
        c->dq[0] = data[2 * k];
        c->dq[1] = data[2 * k + 1];
    }
    c = push(s, ddr3_encode(DDR3_NOP, 0, 0, s->cke));
    if (c) c->flags = CY_DRIVE;                 // postamble
    nops(s, ddr3_ck(s, TWR_NS, 4) - 1);         // tWR
    close_row(s, x);
}

void ddr3_seq_read(ddr3_seq_t *s, uint32_t byte_addr)
{
    ddr3_addr_t x = ddr3_split(byte_addr & ~7u);
    open_row(s, x);
    ddr3_seq_cmd(s, DDR3_RD, x.bank, x.col);
    nops(s, DDR3_RL_DLL_OFF - DDR3_READ_WINDOW_BEFORE - 1);
    s->last_read = s->len;
    for (int k = 0; k < DDR3_READ_WINDOW_CYCLES; k++) {
        ddr3_cycle_t *c = push(s, ddr3_encode(DDR3_NOP, 0, 0, s->cke));
        if (!c) return;
        c->flags = CY_SAMPLE;
    }
    // the window ends past tRTP (4 CK) and tRAS; one more cycle carries the
    // last falling-edge sample point before the PRE
    nops(s, 1);
    close_row(s, x);
}

size_t ddr3_seq_last_read_start(const ddr3_seq_t *s)
{
    return s->last_read;
}

void ddr3_seq_sre(ddr3_seq_t *s)
{
    ddr3_seq_cmd(s, DDR3_PRE, 0, 1u << 10);
    nops(s, ddr3_ck(s, TRP_NS, 1));
    // pay off postponed REFs: JEDEC counts them across self-refresh
    uint32_t trfc = ddr3_ck(s, TRFC_NS, 1);
    while (s->since_ref >= s->ref_interval && !s->overflow)
        ddr3_seq_idle(s, trfc + 1);
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
