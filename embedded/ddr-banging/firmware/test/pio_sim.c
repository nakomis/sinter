#include "pio_sim.h"

#include <epio.h>
#include <epio_pioasm.h>

#include "ddr3_cmd.pio.h"
#include "pins.h"
#include "sim.h"

typedef struct {
    const uint32_t *words;
    size_t n, next;
} stream_t;

static uint8_t feed(epio_t *e, uint8_t block, uint8_t sm, uint32_t *word, void *ctx)
{
    (void)e, (void)block, (void)sm;
    stream_t *s = ctx;
    if (s->next == s->n) return 0;   // DMA has run dry: the SM stalls
    *word = s->words[s->next++];
    return 1;
}

static void record(epio_t *e, uint64_t cycle, uint64_t out, uint64_t oe, uint64_t changed, void *ctx)
{
    (void)e, (void)ctx;
    if (!changed) return;
    sim_t = (long)cycle;
    sim_out = out;
    sim_oe = oe;
    sim_record();
}

bool pio_sim_run_cmd(const uint32_t *cmd, size_t words, uint32_t tck_ns, size_t total_cycles,
                     pio_sim_result_t *out)
{
    // the program has no delay slots: the divider does all the work
    if (!pioenc_timing(tck_ns, PIO_SIM_SYSCLK_HZ, 1, 0, &out->timing)) return false;
    sim_reset();
    sim_out = sim_oe = 0;

    epio_t *e = epio_init();
    epio_set_clkdiv_enabled(e, 1);
    epio_set_input_sync_enabled(e, 1);
    epio_set_sysclk_hz(e, PIO_SIM_SYSCLK_HZ);
    epio_set_gpiobase(e, 0, 16);
    for (uint8_t pin = PIN_CA_BASE; pin < PIN_CA_BASE + CA_BITS; pin++) {
        epio_set_gpio_output_control(e, pin, 0);
        epio_set_gpio_output(e, pin);
    }
    uint off = epio_add_program(e, 0, &ddr3_cmd_program, 0);
    pio_sm_config c = ddr3_cmd_program_get_default_config(off);
    // absolute GPIOs, as with the pico-sdk: epio moves them into the
    // GPIOBASE 16 window (fixed in v0.2.2-nakomis.2, PIOE-12)
    sm_config_set_out_pins(&c, PIN_CA_BASE + CA_CKE, PIOENC_CA_BITS);
    sm_config_set_sideset_pins(&c, PIN_CA_BASE + CA_CK);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_clkdiv_int_frac8(&c, out->timing.div, 0);
    epio_sm_init(e, 0, 0, off, &c);

    stream_t s = { cmd, words, 0 };
    epio_set_dreq_callbacks(e, 0, 0, feed, NULL, &s);
    epio_set_cycle_callback(e, record, NULL);
    epio_write_ctrl(e, 0, (1u << 0) | (1u << 8));   // enable SM0, restart its divider
    for (size_t done = 0; done < total_cycles;) {
        uint32_t n = total_cycles - done > 100000 ? 100000 : (uint32_t)(total_cycles - done);
        epio_step_cycles(e, n);
        done += n;
    }
    out->cycles = total_cycles;
    out->cmd_words_used = s.next;
    epio_free(e);
    return true;
}
