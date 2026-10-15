#include "dram.h"

#include "pico/time.h"

#include "ddr3.h"

#define SEQ_CAP 8192
static ddr3_cycle_t seq_buf[SEQ_CAP];
static bus_sample_t samples[64];
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
    ddr3_seq_t s;
    seq_start(&s);
    ddr3_seq_srx(&s);
    ddr3_seq_write(&s, addr, d);
    ddr3_seq_sre(&s);
    return run(&s);
}

bool dram_read_burst(uint32_t addr, uint8_t out[8])
{
    ddr3_seq_t s;
    seq_start(&s);
    ddr3_seq_srx(&s);
    ddr3_seq_read(&s, addr);
    ddr3_seq_sre(&s);
    if (!run(&s)) return false;
    for (int k = 0; k < 8; k++) {
        unsigned beat = 2 * DDR3_READ_WINDOW_BEFORE + k;
        out[k] = samples[beat / 2].beat[beat % 2] & 0xFF;
    }
    return true;
}

// One self-refresh exit per burst: slow (milestone 11 batches them by row).
bool dram_write(uint32_t addr, const uint8_t *d, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 8)
        if (!dram_write_burst(addr + i, d + i)) return false;
    return true;
}

bool dram_read(uint32_t addr, uint8_t *d, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 8)
        if (!dram_read_burst(addr + i, d + i)) return false;
    return true;
}
