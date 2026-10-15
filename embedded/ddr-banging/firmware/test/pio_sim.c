#include "pio_sim.h"

#include <epio.h>
#include <epio_pioasm.h>

#include "ddr3_cmd.pio.h"
#include "ddr3_smp.pio.h"
#include "ddr3_wr.pio.h"
#include "pins.h"
#include "sim.h"

typedef struct {
    const uint32_t *words;
    size_t n, next;
} stream_t;

typedef struct {
    stream_t cmd, wr, smp;
    uint32_t *rx;
    size_t rx_cap, rx_len;
} ctx_t;

static uint8_t feed(epio_t *e, uint8_t block, uint8_t sm, uint32_t *word, void *c)
{
    (void)e, (void)sm;
    ctx_t *x = c;
    stream_t *s = block == 0 ? &x->cmd : block == 1 ? &x->wr : &x->smp;
    if (s->next == s->n) return 0;   // DMA has run dry: the SM stalls
    *word = s->words[s->next++];
    return 1;
}

static void drain(epio_t *e, uint8_t block, uint8_t sm, uint32_t word, void *c)
{
    (void)e, (void)block, (void)sm;
    ctx_t *x = c;
    if (x->rx_len < x->rx_cap) x->rx[x->rx_len] = word;
    x->rx_len++;
}

static void on_cycle(epio_t *e, uint64_t cycle, uint64_t out, uint64_t oe, uint64_t changed, void *c)
{
    (void)c;
    if (changed) {
        sim_t = (long)cycle;
        sim_out = out;
        sim_oe = oe;
        sim_record();
    }
    // the DRAM side drives DQ/DQS whenever the RP2350 doesn't, from the next cycle
    if (sim_input) {
        uint64_t in = sim_input((long)cycle + 1);
        for (uint8_t pin = 0; pin < 10; pin++)
            if (!((oe >> pin) & 1)) epio_set_gpio_input_level(e, pin, (in >> pin) & 1);
    }
}

bool pio_sim_timing(uint32_t tck_ns, pioenc_timing_t *t)
{
    // each quarter is one instruction of two SM cycles (or two of one)
    return pioenc_timing(tck_ns, PIO_SIM_SYSCLK_HZ, 2, 0, t);
}

static void own(epio_t *e, uint8_t block, uint8_t first, uint8_t last, bool output)
{
    for (uint8_t pin = first; pin <= last; pin++) {
        epio_set_gpio_output_control(e, pin, block);
        if (output) epio_set_gpio_output(e, pin);
        else epio_set_gpio_input(e, pin);
    }
}

bool pio_sim_run(const pio_sim_streams_t *st, uint32_t tck_ns, size_t total_cycles,
                 pio_sim_result_t *out)
{
    if (!pio_sim_timing(tck_ns, &out->timing)) return false;
    uint32_t div = out->timing.div;
    bool data = st->wr && st->smp;
    uint64_t (*input)(long) = sim_input;   // sim_reset() clears it
    sim_reset();
    sim_input = input;
    sim_out = sim_oe = 0;

    epio_t *e = epio_init();
    epio_set_clkdiv_enabled(e, 1);
    epio_set_input_sync_enabled(e, 1);
    epio_set_sysclk_hz(e, PIO_SIM_SYSCLK_HZ);

    // PIO0 SM0: command/address and CK (GPIO 16..40)
    epio_set_gpiobase(e, 0, 16);
    own(e, 0, PIN_CA_BASE, PIN_CA_BASE + CA_BITS - 1, true);
    uint off = epio_add_program(e, 0, &ddr3_cmd_program, 0);
    pio_sm_config c = ddr3_cmd_program_get_default_config(off);
    sm_config_set_out_pins(&c, PIN_CA_BASE + CA_CKE, PIOENC_CA_BITS);
    sm_config_set_sideset_pins(&c, PIN_CA_BASE + CA_CK);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_clkdiv_int_frac8(&c, div, 0);
    epio_sm_init(e, 0, 0, off, &c);

    ctx_t x = { { st->cmd, st->n_cmd, 0 }, { st->wr, st->n_wr, 0 }, { st->smp, st->n_smp, 0 },
                st->rx, st->rx_cap, 0 };
    epio_set_dreq_callbacks(e, 0, 0, feed, NULL, &x);

    if (data) {
        // PIO1 SM0: write data; DQ/DQS directions come from the program, DIR is always out
        own(e, 1, PIN_DQ0, PIN_DQSN, false);
        own(e, 1, PIN_DIR_DQ, PIN_DIR_DQS, true);
        off = epio_add_program(e, 1, &ddr3_wr_program, 0);
        c = ddr3_wr_program_get_default_config(off);
        sm_config_set_out_pins(&c, PIN_DQ0, 10);
        sm_config_set_sideset_pins(&c, PIN_DQS);
        sm_config_set_out_shift(&c, true, true, PIOENC_WORD_BITS);
        sm_config_set_clkdiv_int_frac8(&c, div, 0);
        // start on a dispatch ('out pc, 2'), which lines up with the cmd SM's
        // start-up nop: both reach their first CK's q0 on the same cycle
        epio_sm_init(e, 1, 0, off + ddr3_wr_offset_start, &c);
        epio_set_dreq_callbacks(e, 1, 0, feed, NULL, &x);

        // PIO2 SM0: read sampling
        off = epio_add_program(e, 2, &ddr3_smp_program, 0);
        c = ddr3_smp_program_get_default_config(off);
        sm_config_set_in_pins(&c, PIN_DQ0);
        sm_config_set_in_shift(&c, false, true, 10);
        sm_config_set_out_shift(&c, true, true, PIOENC_WORD_BITS);
        sm_config_set_clkdiv_int_frac8(&c, div, 0);
        epio_sm_init(e, 2, 0, off + ddr3_smp_offset_start, &c);
        epio_set_dreq_callbacks(e, 2, 0, feed, drain, &x);
    }

    // DMA has the FIFOs topped up before the SMs start
    for (int k = 0; k < 4; k++) {
        uint32_t w;
        if (feed(e, 0, 0, &w, &x)) epio_push_tx_fifo(e, 0, 0, w);
        if (data && feed(e, 1, 0, &w, &x)) epio_push_tx_fifo(e, 1, 0, w);
        if (data && feed(e, 2, 0, &w, &x)) epio_push_tx_fifo(e, 2, 0, w);
    }
    epio_set_cycle_callback(e, on_cycle, NULL);
    // PIO0 SM0, and SM0 of PIO1 (next) and PIO2 (prev, wrapping), enabled and
    // their dividers restarted on the same cycle
    uint32_t ctrl = (1u << 0) | (1u << 8);
    if (data) ctrl |= (1u << 20) | (1u << 16) | (1u << 24) | (1u << 26);
    epio_write_ctrl(e, 0, ctrl);
    for (size_t done = 0; done < total_cycles;) {
        uint32_t n = total_cycles - done > 100000 ? 100000 : (uint32_t)(total_cycles - done);
        epio_step_cycles(e, n);
        done += n;
    }
    out->cycles = total_cycles;
    out->cmd_used = x.cmd.next;
    out->wr_used = x.wr.next;
    out->smp_used = x.smp.next;
    out->rx_len = x.rx_len;
    epio_free(e);
    return true;
}
