// An independent JEDEC DDR3 timing checker for the host tests.
//
// The sequence builder (src/ddr3.c) decides when to issue commands; this
// file only knows the rules, from JESD79-3 and the Samsung K4B1G0846F /
// Micron 1 Gb sheets (DDR3-1333, DLL off: CL = CWL = 6, RL = 5). It watches
// every CK rising edge and reports any command that breaks a minimum or
// maximum, so the builder is checked against the datasheet rather than
// against itself.
//
// Two front ends: a built sequence (one cycle per CK, plus the data flags),
// or the simulated pins that bus_run actually drove (test/stubs/sim.h).

#ifndef JEDEC_H
#define JEDEC_H

#include <stdbool.h>
#include <stdint.h>

#include "ddr3.h"

// Violations found; the first few are printed. `in_sr`: the DRAM starts in
// self-refresh (a command sequence) rather than powered up and uninitialised.
int jedec_check_seq(const ddr3_seq_t *s, bool in_sr);
int jedec_check_pins(uint32_t tck_ns, bool in_sr);

// Check against the above-85 C refresh rate (tREFI 3.9 us) from now on.
void jedec_set_hot(bool hot);

#endif
