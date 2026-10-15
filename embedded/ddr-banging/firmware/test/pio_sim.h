// The PIO executor under the host PIO emulator (nakomis/epio): load the
// programs, feed them word streams the way DMA will, and record the pins
// into the same sim_log the JEDEC checker and DRAM model read, at one tick
// per system clock. sim_input, if set, drives the pins PIO isn't driving.

#ifndef PIO_SIM_H
#define PIO_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pio_enc.h"

#define PIO_SIM_SYSCLK_HZ 150000000u

// The executor's timing: 8 SM cycles per CK (4 quarters x 2), integer divider.
bool pio_sim_timing(uint32_t tck_ns, pioenc_timing_t *t);
#define PIO_SIM_SM_CYCLES_PER_CK 8u

typedef struct {
    const uint32_t *cmd, *wr, *smp;   // wr/smp NULL: command SM only
    size_t n_cmd, n_wr, n_smp;
    uint32_t *rx;                     // samples, one 10-bit word each
    size_t rx_cap;
} pio_sim_streams_t;

typedef struct {
    pioenc_timing_t timing;
    uint64_t cycles;                  // system clocks stepped
    size_t cmd_used, wr_used, smp_used, rx_len;
} pio_sim_result_t;

// Play the streams in lock-step (PIO0/1/2 SM0, started on one cycle) for
// total_cycles system clocks, recording the pins (sim_reset() first).
bool pio_sim_run(const pio_sim_streams_t *st, uint32_t tck_ns, size_t total_cycles,
                 pio_sim_result_t *out);

#endif
