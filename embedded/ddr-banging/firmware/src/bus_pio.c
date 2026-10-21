#include "bus_pio.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "ddr3_cap.pio.h"
#include "ddr3_cmd.pio.h"
#include "ddr3_smp.pio.h"
#include "ddr3_wr.pio.h"
#include "pins.h"
#include "pio_enc.h"

// One SM per block, SM 0 each (EXECUTOR.md section 2). Proven under epio by
// test/test_pio_cmd.c and test/test_pio_data.c, with the same programs and
// the same configuration as test/pio_sim.c; keep the two in step.
#define CMD_PIO pio0   // GPIOBASE 16
#define WR_PIO  pio1
#define SMP_PIO pio2
#define SM      0

#define STREAM_CAP 8192   // words per stream: at worst one per cycle (dram.c's SEQ_CAP)

#define CA_MASK64  (((1ull << CA_BITS) - 1) << PIN_CA_BASE)
#define DQ_MASK64  (0x3FFull << PIN_DQ0)          // DQ0-7, DQS, DQS#
#define DIR_MASK64 ((1ull << PIN_DIR_DQ) | (1ull << PIN_DIR_DQS))

static uint32_t cmd_w[STREAM_CAP], wr_w[STREAM_CAP], smp_w[STREAM_CAP];
static uint off_cmd, off_wr, off_smp, off_cap;
static int ch_cmd, ch_wr, ch_smp, ch_rx, ch_cap;
#define CAP_SM 1   // PIO2 SM1: the self-check's capture (pio_check.h)
static uint32_t tck_ns, div;
static const char *error;

const char *bus_pio_error(void) { return error; }

static uint32_t pio_tck(void) { return tck_ns; }

static uint32_t pio_set_tck(uint32_t ns)
{
    // 8 SM cycles per CK (4 quarters of 2), integer divider: 53.3 ns steps at
    // 150 MHz. Rounds down (never slower than asked: the refresh cap is a
    // maximum period), and never below div 1.
    pioenc_timing_t t;
    pioenc_executor_timing(ns, clock_get_hz(clk_sys), &t);
    div = t.div;
    tck_ns = (t.tck_ps + 999) / 1000;
    pio_sm_set_clkdiv_int_frac8(CMD_PIO, SM, div, 0);
    pio_sm_set_clkdiv_int_frac8(WR_PIO, SM, div, 0);
    pio_sm_set_clkdiv_int_frac8(SMP_PIO, SM, div, 0);
    pio_sm_set_clkdiv_int_frac8(SMP_PIO, CAP_SM, div, 0);
    return tck_ns;
}

void bus_pio_init(void)
{
    pio_set_gpio_base(CMD_PIO, 16);
    pio_sm_claim(CMD_PIO, SM);
    pio_sm_claim(WR_PIO, SM);
    pio_sm_claim(SMP_PIO, SM);

    off_cmd = pio_add_program(CMD_PIO, &ddr3_cmd_program);
    pio_sm_config c = ddr3_cmd_program_get_default_config(off_cmd);
    sm_config_set_out_pins(&c, PIN_CA_BASE + CA_CKE, PIOENC_CA_BITS);
    sm_config_set_sideset_pins(&c, PIN_CA_BASE + CA_CK);
    sm_config_set_out_shift(&c, true, true, 32);
    pio_sm_init(CMD_PIO, SM, off_cmd + ddr3_cmd_offset_start, &c);

    off_wr = pio_add_program(WR_PIO, &ddr3_wr_program);   // .origin 0
    c = ddr3_wr_program_get_default_config(off_wr);
    sm_config_set_out_pins(&c, PIN_DQ0, 10);
    sm_config_set_sideset_pins(&c, PIN_DQS);
    sm_config_set_out_shift(&c, true, true, PIOENC_WORD_BITS);
    pio_sm_init(WR_PIO, SM, off_wr + ddr3_wr_offset_start, &c);

    off_smp = pio_add_program(SMP_PIO, &ddr3_smp_program);   // .origin 0
    c = ddr3_smp_program_get_default_config(off_smp);
    sm_config_set_in_pins(&c, PIN_DQ0);
    sm_config_set_in_shift(&c, false, true, 10);
    sm_config_set_out_shift(&c, true, true, PIOENC_WORD_BITS);
    pio_sm_init(SMP_PIO, SM, off_smp + ddr3_smp_offset_start, &c);

    pio_sm_claim(SMP_PIO, CAP_SM);
    off_cap = pio_add_program(SMP_PIO, &ddr3_cap_program);   // after the sampler
    c = ddr3_cap_program_get_default_config(off_cap);
    sm_config_set_in_pins(&c, 0);
    sm_config_set_in_shift(&c, false, true, 32);
    pio_sm_init(SMP_PIO, CAP_SM, off_cap, &c);

    ch_cmd = dma_claim_unused_channel(true);
    ch_wr = dma_claim_unused_channel(true);
    ch_smp = dma_claim_unused_channel(true);
    ch_rx = dma_claim_unused_channel(true);
    ch_cap = dma_claim_unused_channel(true);
    pio_set_tck(1000);
}

void bus_pio_take_pins(void)
{
    // PIO starts from the levels the CPU is driving now, then takes the pads
    uint64_t now = gpio_get_all64();
    pio_sm_set_pins_with_mask64(CMD_PIO, SM, now, CA_MASK64);
    pio_sm_set_pindirs_with_mask64(CMD_PIO, SM, CA_MASK64, CA_MASK64);
    pio_sm_set_pins_with_mask64(WR_PIO, SM, now, DIR_MASK64);
    pio_sm_set_pindirs_with_mask64(WR_PIO, SM, DIR_MASK64, DIR_MASK64 | DQ_MASK64);
    for (uint pin = PIN_CA_BASE; pin < PIN_CA_BASE + CA_BITS; pin++) pio_gpio_init(CMD_PIO, pin);
    for (uint pin = PIN_DQ0; pin <= PIN_DIR_DQS; pin++) pio_gpio_init(WR_PIO, pin);
}

void bus_pio_give_pins(void)
{
    // the CPU's output latches take the levels PIO left, then the pads
    // DIR goes back low regardless: the bit-banger only drives the translators
    // when it means to (its drive state is "off")
    uint64_t mask = CA_MASK64 | DQ_MASK64 | DIR_MASK64;
    gpio_put_masked64(mask, gpio_get_all64() & ~DIR_MASK64);
    for (uint pin = PIN_DQ0; pin <= PIN_DIR_DQS; pin++) gpio_set_function(pin, GPIO_FUNC_SIO);
    for (uint pin = PIN_CA_BASE; pin < PIN_CA_BASE + CA_BITS; pin++) gpio_set_function(pin, GPIO_FUNC_SIO);
}

// Disabled, FIFOs and shift registers empty, at its starting instruction
static void rewind_sm(PIO p, uint sm, uint pc)
{
    pio_sm_set_enabled(p, sm, false);
    pio_sm_clear_fifos(p, sm);
    pio_sm_restart(p, sm);
    pio_sm_exec(p, sm, pio_encode_jmp(pc));
    p->fdebug = (1u << (PIO_FDEBUG_TXSTALL_LSB + sm)) | (1u << (PIO_FDEBUG_RXSTALL_LSB + sm));
}

static void rewind(PIO p, uint pc)
{
    rewind_sm(p, SM, pc);
}

static void feed(int ch, PIO p, const uint32_t *words, size_t n)
{
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(p, SM, true));
    dma_channel_configure(ch, &c, &p->txf[SM], words, n, true);
}

static bool stalled(PIO p)
{
    return (p->fdebug >> (PIO_FDEBUG_TXSTALL_LSB + SM)) & 1;
}

static void abort_channel(int ch)
{
    // RP2350-E5: clear EN before aborting, or the abort can re-trigger it
    hw_clear_bits(&dma_hw->ch[ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(ch);
}

static void stop(bool failed)
{
    pio_sm_set_enabled(CMD_PIO, SM, false);
    pio_sm_set_enabled(WR_PIO, SM, false);
    pio_sm_set_enabled(SMP_PIO, SM, false);
    pio_sm_set_enabled(SMP_PIO, CAP_SM, false);
    abort_channel(ch_cmd);
    abort_channel(ch_wr);
    abort_channel(ch_smp);
    abort_channel(ch_rx);
    abort_channel(ch_cap);
    if (failed) {
        // wherever it stopped: RP2350 off DQ/DQS first, then the translators
        // back to DRAM -> RP2350, so nothing fights whatever comes next
        pio_sm_set_pindirs_with_mask64(WR_PIO, SM, 0, DQ_MASK64);
        pio_sm_set_pins_with_mask64(WR_PIO, SM, 0, DIR_MASK64);
    }
}

// An SM that stalled while its stream still had words in flight ran dry
// mid-sequence (DMA too slow): the three are no longer in step. The sampler
// stalling on a full RX FIFO is the same.
static bool underran(PIO p, int ch)
{
    return stalled(p) && (dma_channel_is_busy(ch) || !pio_sm_is_tx_fifo_empty(p, SM));
}

static bool run(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max_samples,
                uint32_t *capture, size_t n_capture)
{
    error = NULL;
    bool o1, o2, o3;
    size_t nc = pioenc_cmd(s->cycles, s->len, cmd_w, STREAM_CAP, &o1);
    size_t nw = pioenc_writer(s->cycles, s->len, wr_w, STREAM_CAP, &o2);
    size_t ns = pioenc_sampler(s->cycles, s->len, smp_w, STREAM_CAP, &o3);
    if (o1 || o2 || o3) {
        error = "sequence too long for the PIO streams";
        return false;
    }
    unsigned want = 0;
    for (size_t i = 0; i < s->len; i++) want += (s->cycles[i].flags & CY_SAMPLE) != 0;
    if (want > max_samples) {
        error = "more samples than the buffer holds";   // the sampler would stall and drift
        return false;
    }

    rewind(CMD_PIO, off_cmd + ddr3_cmd_offset_start);   // its one-off start-up pull + nop
    rewind(WR_PIO, off_wr + ddr3_wr_offset_start);
    rewind(SMP_PIO, off_smp + ddr3_smp_offset_start);
    if (capture) {
        rewind_sm(SMP_PIO, CAP_SM, off_cap);
        dma_channel_config c = dma_channel_get_default_config(ch_cap);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(SMP_PIO, CAP_SM, false));
        dma_channel_configure(ch_cap, &c, capture, &SMP_PIO->rxf[CAP_SM], n_capture, true);
    }

    // samples: two beats per sampled cycle, 16 bits each, straight into bus_sample_t
    if (want) {
        dma_channel_config c = dma_channel_get_default_config(ch_rx);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(SMP_PIO, SM, false));
        dma_channel_configure(ch_rx, &c, samples, &SMP_PIO->rxf[SM], 2 * want, true);
    }
    feed(ch_cmd, CMD_PIO, cmd_w, nc);
    feed(ch_wr, WR_PIO, wr_w, nw);
    feed(ch_smp, SMP_PIO, smp_w, ns);
    // FIFOs full (or the whole stream in) before the start: the three SMs'
    // alignment on CK 0 assumes their first OUTs find data
    while ((!pio_sm_is_tx_fifo_full(CMD_PIO, SM) && dma_channel_is_busy(ch_cmd)) ||
           (!pio_sm_is_tx_fifo_full(WR_PIO, SM) && dma_channel_is_busy(ch_wr)) ||
           (!pio_sm_is_tx_fifo_full(SMP_PIO, SM) && dma_channel_is_busy(ch_smp)))
        tight_loop_contents();

    // SM 0 of PIO0, of PIO1 (next) and PIO2 (previous, wrapping round),
    // enabled and their dividers restarted on one cycle
    uint64_t t0 = time_us_64();
    uint32_t prev = (1u << SM) | (capture ? 1u << CAP_SM : 0);
    pio_enable_sm_multi_mask_in_sync(CMD_PIO, prev, 1u << SM, 1u << SM);

    // The three SMs are deterministic: the sequence takes exactly len x tCK
    // from the start. Done once that (plus two cycles' grace) has passed and
    // every stream is spent and every sample is in; by then each SM has
    // stalled on its empty FIFO, the cmd SM with CK low and the DRAM in
    // self-refresh. (Not FDEBUG TXSTALL: whether it re-asserts while an SM
    // stays stalled isn't documented.) Twice the time, plus 1 ms, is a fault.
    uint64_t need_us = ((uint64_t)(s->len + 2) * tck_ns + 999) / 1000;
    uint64_t limit = t0 + 2 * need_us + 1000;
    for (;;) {
        uint64_t now = time_us_64();
        bool fed = !dma_channel_is_busy(ch_cmd) && !dma_channel_is_busy(ch_wr) &&
                   !dma_channel_is_busy(ch_smp) && pio_sm_is_tx_fifo_empty(CMD_PIO, SM) &&
                   pio_sm_is_tx_fifo_empty(WR_PIO, SM) && pio_sm_is_tx_fifo_empty(SMP_PIO, SM);
        if (underran(CMD_PIO, ch_cmd) || underran(WR_PIO, ch_wr) || underran(SMP_PIO, ch_smp) ||
            ((SMP_PIO->fdebug >> (PIO_FDEBUG_RXSTALL_LSB + SM)) & 1)) {
            error = "a state machine ran dry mid-sequence (DMA underrun)";
            break;
        }
        if (fed && now - t0 >= need_us && (!want || !dma_channel_is_busy(ch_rx)) &&
            (!capture || !dma_channel_is_busy(ch_cap))) {
            // every SM is now waiting for a word that will never come
            if (!stalled(CMD_PIO)) error = "command SM still running after its stream";
            break;
        }
        if (now > limit) {
            error = "timed out";
            break;
        }
    }
    stop(error != NULL);
    return error == NULL;
}

static bool pio_run(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max_samples)
{
    return run(s, samples, max_samples, NULL, 0);
}

bool bus_pio_run_captured(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max_samples,
                          uint32_t *capture, size_t n_capture)
{
    return run(s, samples, max_samples, capture, n_capture);
}

const bus_ops_t bus_pio_ops = { "pio (3 state machines + DMA)", pio_run, pio_tck, pio_set_tck };
