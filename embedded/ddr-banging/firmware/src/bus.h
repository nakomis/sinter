// A bus executor: whatever plays a ddr3_seq_t onto the pins. Two exist:
// the CPU bit-banger (bus_gpio.c, the default and bring-up path) and the
// PIO executor (bus_pio.c, ../EXECUTOR.md). dram.c uses whichever is
// selected; switching is only safe while the DRAM is in self-refresh.

#ifndef BUS_H
#define BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "bus_gpio.h"   // bus_sample_t
#include "ddr3.h"

typedef struct {
    const char *name;
    // Play s; each CY_SAMPLE cycle fills one bus_sample_t. False if the
    // executor couldn't (stream or sample buffer too small, timeout).
    bool (*run)(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max_samples);
    uint32_t (*tck)(void);                 // the clock period actually in use (ns)
    uint32_t (*set_tck)(uint32_t tck_ns);  // aim for this; returns what it got
} bus_ops_t;

extern const bus_ops_t bus_gpio_ops;

#endif
