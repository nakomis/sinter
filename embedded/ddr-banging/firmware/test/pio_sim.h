// The PIO executor under the host PIO emulator (nakomis/epio): load the
// programs, feed them word streams the way DMA will, and record the pins
// into the same sim_log the JEDEC checker and DRAM model read, at one tick
// per system clock.

#ifndef PIO_SIM_H
#define PIO_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pio_enc.h"

#define PIO_SIM_SYSCLK_HZ 150000000u

typedef struct {
    pioenc_timing_t timing;   // what the SMs ran at
    uint64_t cycles;          // system clocks stepped
    size_t cmd_words_used;    // words the command SM took
} pio_sim_result_t;

// Play a command word stream on the command SM (PIO0 SM0, GPIOBASE 16) at
// the tCK nearest tck_ns, for long enough to finish it, recording the pins
// (sim_reset() first). False if the timing can't be met.
bool pio_sim_run_cmd(const uint32_t *cmd, size_t words, uint32_t tck_ns, size_t total_cycles,
                     pio_sim_result_t *out);

#endif
