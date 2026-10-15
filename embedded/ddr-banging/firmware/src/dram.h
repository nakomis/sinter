// The DRAM as the shell and the ESP32 link see it: init, and reads and writes
// of whole bursts. Between operations the DRAM waits in self-refresh.

#ifndef DRAM_H
#define DRAM_H

#include <stdbool.h>
#include <stdint.h>

#include "bus_gpio.h"

bool dram_initialised(void);
void dram_forget(void);    // the bus was driven by hand (hold/walk): init again

// RESET#, DLL-off init, self-refresh.
bool dram_init(void);

// One burst (8 bytes at a burst-aligned address). The read keeps its sample
// window: dram_samples() returns it, *n set to the cycles sampled.
bool dram_write_burst(uint32_t addr, const uint8_t d[8]);
bool dram_read_burst(uint32_t addr, uint8_t out[8]);
const bus_sample_t *dram_samples(unsigned *n);

// Whole multiples of 8 bytes, burst by burst.
bool dram_write(uint32_t addr, const uint8_t *d, uint32_t len);
bool dram_read(uint32_t addr, uint8_t *d, uint32_t len);

// Last failure, for the shell to print; NULL if none.
const char *dram_error(void);

#endif
