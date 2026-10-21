// The PIO executor (../EXECUTOR.md): three state machines, one per PIO block,
// fed by DMA, playing the same ddr3_seq_t as the bit-banger. Exposed as
// bus_pio_ops for dram_set_bus().

#ifndef BUS_PIO_H
#define BUS_PIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus.h"

extern const bus_ops_t bus_pio_ops;

// Load the programs, claim the SMs and DMA channels. Once, at boot; the pins
// stay with the bit-banger until bus_pio_take_pins().
void bus_pio_init(void);

// Hand GPIO 0-11 and 16-40 to PIO, or back to the CPU (SIO). Each side takes
// over the levels the pins have now, so nothing glitches; the DRAM must be
// in self-refresh (between operations).
void bus_pio_take_pins(void);
void bus_pio_give_pins(void);

// A run with the self-check's capture SM (PIO2 SM1) alongside: capture gets
// GPIO 0-31 on each SM cycle from the synchronised start (pio_check.h).
bool bus_pio_run_captured(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max_samples,
                          uint32_t *capture, size_t n_capture);

// Why the last run failed, or NULL.
const char *bus_pio_error(void);

#endif
