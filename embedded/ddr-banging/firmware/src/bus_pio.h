// The PIO executor (../EXECUTOR.md): three state machines, one per PIO block,
// fed by DMA, playing the same ddr3_seq_t as the bit-banger. Exposed as
// bus_pio_ops for dram_set_bus().

#ifndef BUS_PIO_H
#define BUS_PIO_H

#include <stdbool.h>

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

// Why the last run failed, or NULL.
const char *bus_pio_error(void);

#endif
