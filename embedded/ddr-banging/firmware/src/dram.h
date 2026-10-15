// The DRAM as the shell and the ESP32 link see it: init, and reads and writes
// of whole bursts. Between operations the DRAM waits in self-refresh.

#ifndef DRAM_H
#define DRAM_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

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

// ---- Temperature ----------------------------------------------------------
// Above 85 C Tcase, DDR3 needs refreshing twice as often, and self-refresh
// only holds data if MR2's SRT bit is set, which is allowed only for chips
// with the extended range (SPD byte 31). Before every operation the module
// temperature is read; from DRAM_HOT_C16 (with margin for sensor accuracy and
// case offset) the refresh interval halves and tCK is capped at
// DDR3_TCK_MAX_HOT_NS, until it falls below DRAM_COOL_C16. Hot without the
// extended range: operations fail rather than risk the data.
#define DRAM_HOT_C16  (80 * 16)
#define DRAM_COOL_C16 (78 * 16)

// Reads the module temperature in 1/16 C; false if the read fails. Installing
// one starts cool; a failed read keeps the last state, so a stick with no
// sensor stays cool and a flaky read while hot stays hot.
void dram_set_thermometer(bool (*read16)(int *t16));
// From the SPD, before dram_init: sets MR2 SRT.
void dram_set_extended_temp(bool supported);
bool dram_hot(void);

// ---- Executor ---------------------------------------------------------------
// bus_gpio_ops (default) or bus_pio_ops. Switch only between operations.
void dram_set_bus(const bus_ops_t *ops);
const bus_ops_t *dram_bus(void);

#endif
