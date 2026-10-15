// A behavioural DDR3 data model for the simulated pins: enough of a DRAM to
// store what the firmware writes and give it back, so dram_write/dram_read
// are tested end to end through bus_run. Timing is checked elsewhere
// (jedec.c); this only answers "did the bytes land, and come back, in the
// right places?".
//
// It reads the pins from sim_log as bus_run records them: commands on CK
// rising edges, write data on DQS edges from WL, and it drives read data from
// RL, one beat per half clock, DQS high on even beats.

#include "dram_model.h"

#include <string.h>

#include "ddr3.h"
#include "pins.h"
#include "sim.h"

#define MODEL_BYTES (1u << 20)   // rows 0..127 of every bank
static long Q = 4;               // sim ticks per CK: 4 under bus_run

static uint8_t mem[MODEL_BYTES];
static size_t seen;              // sim_log entries processed
static uint64_t prev_out;
static unsigned open_row[8];
static bool out_of_range;

typedef struct { long start; uint32_t addr; } xfer_t;   // start: tick of beat 0
#define PENDING 32
static xfer_t reads[PENDING], writes[PENDING];
static unsigned n_reads, n_writes;

void model_set_ticks_per_ck(long ticks)
{
    Q = ticks;
}

void model_rewind(void)
{
    seen = 0;
    prev_out = 0;
    n_reads = n_writes = 0;
}

void model_reset(void)
{
    Q = 4;
    memset(mem, 0, sizeof mem);
    seen = 0;
    prev_out = 0;
    n_reads = n_writes = 0;
    out_of_range = false;
}


static uint32_t linear(unsigned bank, unsigned row, unsigned col)
{
    return (uint32_t)row << (DDR3_COL_BITS + DDR3_BANK_BITS) | bank << DDR3_COL_BITS | col;
}

static void push(xfer_t *q, unsigned *n, long start, uint32_t addr)
{
    if (*n == PENDING) memmove(q, q + 1, (PENDING - 1) * sizeof *q), (*n)--;
    q[(*n)++] = (xfer_t){ start, addr };
}

static void command(long t, uint32_t ca)
{
    if (!((ca >> CA_CKE) & 1) || ((ca >> CA_CS0N) & 1)) return;
    unsigned ras = (ca >> CA_RASN) & 1, cas = (ca >> CA_CASN) & 1, we = (ca >> CA_WEN) & 1;
    unsigned ba = (ca >> CA_BA0) & 7, a = (ca >> CA_A0) & 0x3FFF;
    if (!ras && cas && we) open_row[ba] = a;                                   // ACT
    else if (ras && !cas) {                                                    // RD / WR
        uint32_t addr = linear(ba, open_row[ba], a & 0x3FF);
        if (we) push(reads, &n_reads, t + Q * DDR3_RL_DLL_OFF, addr);
        else push(writes, &n_writes, t + Q * DDR3_CWL, addr);
    }
}

static void strobe_edge(long t, uint8_t dq)
{
    for (unsigned i = 0; i < n_writes; i++) {
        long half = Q / 2, k = (t - writes[i].start + half / 2) / half;   // edges at start + k * half
        if (t >= writes[i].start - half / 2 && k < 8) {
            uint32_t a = writes[i].addr + (uint32_t)k;
            if (a < MODEL_BYTES) mem[a] = dq;
            else out_of_range = true;
            return;
        }
    }
}

static void catch_up(void)
{
    const uint64_t ck = 1ull << (PIN_CA_BASE + CA_CK), dqs = 1ull << PIN_DQS;
    for (; seen < sim_len; seen++) {
        const sim_snap_t *e = &sim_log[seen];
        if ((e->out & ck) && !(prev_out & ck))
            command(e->t, (uint32_t)(e->out >> PIN_CA_BASE));
        if ((e->oe & dqs) && ((e->out ^ prev_out) & dqs))
            strobe_edge(e->t, (uint8_t)e->out);
        prev_out = e->out;
    }
}

uint8_t model_byte(uint32_t addr)
{
    catch_up();   // writes never sample an input, so the log may be unread
    return addr < MODEL_BYTES ? mem[addr] : 0;
}

bool model_out_of_range(void)
{
    catch_up();
    return out_of_range;
}

uint64_t model_input(long t)
{
    catch_up();
    for (unsigned i = 0; i < n_reads; i++) {
        long d = t - reads[i].start;
        if (d >= 0 && d < 4 * Q) {
            unsigned k = (unsigned)(d / (Q / 2));
            uint32_t a = reads[i].addr + k;
            uint64_t v = a < MODEL_BYTES ? mem[a] : 0;
            return v | (k % 2 ? 1ull << PIN_DQSN : 1ull << PIN_DQS);
        }
    }
    return 1ull << PIN_DQSN;   // not driving: the translator idles at DQS low
}
