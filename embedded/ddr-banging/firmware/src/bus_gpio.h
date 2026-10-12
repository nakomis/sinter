// First-light executor: plays a ddr3_seq_t out on the GPIOs by bit-banging,
// with the CPU timing each quarter of the clock. Slow (≤ a few MHz) but easy
// to follow on a scope or logic analyser.

#ifndef BUS_GPIO_H
#define BUS_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "ddr3.h"

// Per sampled cycle: GPIO 0..9 (DQ0-7, DQS, DQS#) mid-high and mid-low after.
typedef struct { uint16_t beat[2]; } bus_sample_t;

void bus_init(void);

// Aim for this clock period; returns the period actually measured (ns).
uint32_t bus_set_tck(uint32_t tck_ns);
uint32_t bus_tck(void);

void bus_reset(bool asserted);

// Play a sequence with interrupts off. Each CY_SAMPLE cycle appends one
// bus_sample_t to `samples` (if non-NULL). Returns elapsed microseconds.
uint32_t bus_run(const ddr3_seq_t *s, bus_sample_t *samples, unsigned max_samples);

// Static levels for continuity checks: all lines idle-low except `gpio`.
// A DQ/DQS line is driven through its translator (DIR set to A->B).
void bus_hold(int gpio);  // -1 releases everything back to idle

#endif
