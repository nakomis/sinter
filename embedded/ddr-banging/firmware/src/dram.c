#include "dram.h"

#include "pico/time.h"

#include "ddr3.h"

#define SEQ_CAP 8192
// Bursts per sequence in dram_read/dram_write: one self-refresh exit (~520
// CK) is shared by all of them, at ~18 CK each plus REFs. 256 fit SEQ_CAP
// up to tck 2 us; slower clocks owe more REFs per burst, so a sequence that
// overflows is rebuilt with half as many.
#define RUN_BURSTS 256
static ddr3_cycle_t seq_buf[SEQ_CAP];
static bus_sample_t samples[RUN_BURSTS * DDR3_READ_WINDOW_CYCLES];
static unsigned n_samples;
static bool initialised;
static const char *error;

bool dram_initialised(void) { return initialised; }
void dram_forget(void) { initialised = false; }
const char *dram_error(void) { return error; }

const bus_sample_t *dram_samples(unsigned *n)
{
    *n = n_samples;
    return samples;
}

static void seq_start(ddr3_seq_t *s)
{
    ddr3_seq_init(s, seq_buf, SEQ_CAP, bus_tck());
    s->cke = initialised;
}

static bool run(ddr3_seq_t *s)
{
    if (s->overflow) {
        error = "sequence too long for the buffer";
        return false;
    }
    bus_run(s, samples, sizeof samples / sizeof samples[0]);
    n_samples = 0;
    for (size_t i = 0; i < s->len; i++) n_samples += (s->cycles[i].flags & CY_SAMPLE) != 0;
    error = NULL;
    return true;
}

bool dram_init(void)
{
    // RESET# low >= 200 us with CKE low, then 500 us before CKE goes high
    initialised = false;
    bus_reset(true);
    sleep_us(200);
    bus_reset(false);
    sleep_us(500);
    ddr3_seq_t s;
    seq_start(&s);
    ddr3_seq_init_mrs(&s);
    ddr3_seq_sre(&s);
    if (!run(&s)) return false;
    initialised = true;
    return true;
}

bool dram_write_burst(uint32_t addr, const uint8_t d[8])
{
    return dram_write(addr, d, 8);
}

// Read j's data, from its sample window (each read samples
// DDR3_READ_WINDOW_CYCLES cycles, in order).
static void unpack_read(unsigned j, uint8_t out[8])
{
    const bus_sample_t *w = samples + j * DDR3_READ_WINDOW_CYCLES;
    for (int k = 0; k < 8; k++) {
        unsigned beat = 2 * DDR3_READ_WINDOW_BEFORE + k;
        out[k] = w[beat / 2].beat[beat % 2] & 0xFF;
    }
}

bool dram_read_burst(uint32_t addr, uint8_t out[8])
{
    return dram_read(addr, out, 8);
}

// One sequence: self-refresh exit, n bursts (each opening and closing its
// own row, then paying the REFs it owes), self-refresh entry. Halves n until
// the sequence fits. Milestone 11: several bursts per open row, within tRAS max.
static bool batch(ddr3_seq_t *s, uint32_t addr, const uint8_t *wr, uint32_t *n)
{
    for (;;) {
        seq_start(s);
        ddr3_seq_srx(s);
        for (uint32_t j = 0; j < *n; j++) {
            if (wr) ddr3_seq_write(s, addr + 8 * j, wr + 8 * j);
            else ddr3_seq_read(s, addr + 8 * j);
            ddr3_seq_catch_up(s);
        }
        ddr3_seq_sre(s);
        if (!s->overflow || *n == 1) return run(s);
        *n /= 2;
    }
}

bool dram_write(uint32_t addr, const uint8_t *d, uint32_t len)
{
    for (uint32_t done = 0; done < len;) {
        uint32_t n = (len - done) / 8 < RUN_BURSTS ? (len - done) / 8 : RUN_BURSTS;
        ddr3_seq_t s;
        if (!batch(&s, addr + done, d + done, &n)) return false;
        done += 8 * n;
    }
    return true;
}

bool dram_read(uint32_t addr, uint8_t *d, uint32_t len)
{
    for (uint32_t done = 0; done < len;) {
        uint32_t n = (len - done) / 8 < RUN_BURSTS ? (len - done) / 8 : RUN_BURSTS;
        ddr3_seq_t s;
        if (!batch(&s, addr + done, NULL, &n)) return false;
        for (uint32_t j = 0; j < n; j++) unpack_read(j, d + done + 8 * j);
        done += 8 * n;
    }
    return true;
}
