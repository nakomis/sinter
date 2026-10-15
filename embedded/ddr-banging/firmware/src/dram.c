#include "dram.h"

#include "pico/time.h"

#include "ddr3.h"

#define SEQ_CAP 8192
// Bursts per sequence in dram_read/dram_write: one self-refresh exit (~520
// CK) is shared by all of them, in runs of up to ddr3_seq_row_bursts() per
// open row (~4 CK each, plus ~18 per run, plus REFs). A sequence that
// overflows SEQ_CAP is rebuilt with half as many.
#define RUN_BURSTS 256
static ddr3_cycle_t seq_buf[SEQ_CAP];
static bus_sample_t samples[RUN_BURSTS * DDR3_READ_WINDOW_CYCLES];
static unsigned n_samples;
static bool initialised;
static const char *error;
static bool (*thermometer)(int *t16);
static bool extended_temp, hot;

void dram_set_thermometer(bool (*read16)(int *t16))
{
    thermometer = read16;
    hot = false;
}
bool dram_hot(void) { return hot; }

void dram_set_extended_temp(bool supported)
{
    extended_temp = supported;
    ddr3_set_srt(supported);
}

// Before every operation: track hot/cool with hysteresis, and make the clock
// legal for the refresh rate. The DRAM is in self-refresh (or not yet
// initialised) here, so changing tCK is allowed.
static bool thermal_ok(void)
{
    int t;
    // a failed reading keeps the last state: a flaky bus while hot stays hot
    if (thermometer && thermometer(&t)) hot = t >= (hot ? DRAM_COOL_C16 : DRAM_HOT_C16);
    if (!hot) return true;
    if (!extended_temp) {
        error = "module at 80 C or more, and its SPD doesn't claim the extended range";
        return false;
    }
    if (bus_tck() > DDR3_TCK_MAX_HOT_NS) bus_set_tck(DDR3_TCK_MAX_HOT_NS);
    if (bus_tck() > DDR3_TCK_MAX_HOT_NS) {
        error = "hot, and the clock can't be brought within the 2x refresh limit";
        return false;
    }
    return true;
}

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
    ddr3_seq_set_hot(s, hot);
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
    if (!thermal_ok()) return false;
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

bool dram_read_burst(uint32_t addr, uint8_t out[8])
{
    return dram_read(addr, out, 8);
}

// One sequence: self-refresh exit, then runs of bursts (one ACT and PRE per
// run, up to ddr3_seq_row_bursts() per row), then self-refresh entry. Halves
// *n until the sequence fits. Read runs' windows are recorded so the bytes
// can be picked out afterwards.
static struct { uint32_t first_sample; unsigned bursts; } runs[RUN_BURSTS];
static unsigned n_runs;

static bool batch(ddr3_seq_t *s, uint32_t addr, const uint8_t *wr, uint32_t *n)
{
    for (;;) {
        seq_start(s);
        ddr3_seq_srx(s);
        unsigned per_row = ddr3_seq_row_bursts(s);
        uint32_t sampled = 0;
        n_runs = 0;
        for (uint32_t j = 0; j < *n && !s->overflow;) {
            unsigned want = *n - j < per_row ? *n - j : per_row;
            unsigned got = wr ? ddr3_seq_write_run(s, addr + 8 * j, wr + 8 * j, want)
                              : ddr3_seq_read_run(s, addr + 8 * j, want);
            if (!wr) {
                runs[n_runs].first_sample = sampled;
                runs[n_runs++].bursts = got;
                sampled += ddr3_read_run_samples(got);
            }
            j += got;
        }
        ddr3_seq_sre(s);
        if (!s->overflow || *n == 1) return run(s);
        *n /= 2;
    }
}

// Burst k of a read run starts DDR3_READ_WINDOW_BEFORE + 4k cycles into its window.
static void unpack_reads(uint8_t *out)
{
    for (unsigned r = 0; r < n_runs; r++)
        for (unsigned k = 0; k < runs[r].bursts; k++, out += 8) {
            const bus_sample_t *w = samples + runs[r].first_sample + DDR3_READ_WINDOW_BEFORE + 4 * k;
            for (int b = 0; b < 8; b++) out[b] = w[b / 2].beat[b % 2] & 0xFF;
        }
}

bool dram_write(uint32_t addr, const uint8_t *d, uint32_t len)
{
    if (!thermal_ok()) return false;
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
    if (!thermal_ok()) return false;
    for (uint32_t done = 0; done < len;) {
        uint32_t n = (len - done) / 8 < RUN_BURSTS ? (len - done) / 8 : RUN_BURSTS;
        ddr3_seq_t s;
        if (!batch(&s, addr + done, NULL, &n)) return false;
        unpack_reads(d + done);
        done += 8 * n;
    }
    return true;
}
