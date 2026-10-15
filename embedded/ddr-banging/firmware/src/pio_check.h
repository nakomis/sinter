// PIO executor self-check (the shell's 'selftest'): what every pin should do
// on every SM cycle, compared with a capture of GPIO 0-31 that a fourth SM
// (PIO2 SM1, 'in pins, 32' each SM cycle) takes alongside the run. Needs no
// DRAM: PIO pads read back their own outputs. Pure logic, shared by the
// firmware and the host tests (which run it under epio).
//
// It proves, on silicon, what EXECUTOR.md section 7 lists as unverified:
// the three SMs start in step (C/A, CK and the writer's pins all line up),
// PIO0's PREV mask reaches PIO2 (the capture exists), and 16-bit DMA reads
// of the RX FIFO pop one entry each (the sampler's loopback matches).

#ifndef PIO_CHECK_H
#define PIO_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus_gpio.h"   // bus_sample_t
#include "ddr3.h"

#define PIO_CHECK_CYCLES 64          // CK cycles in the check sequence
#define PIO_CHECK_SM_PER_CK 8
#define PIO_CHECK_CAPTURE (PIO_CHECK_CYCLES * PIO_CHECK_SM_PER_CK + 32)   // words

// The check sequence: CKE low throughout (any DRAM attached ignores it; hold
// RESET# low too), C/A patterns that exercise every visible pin and the
// repeat path, and runs of write bursts that are also sampled, so the
// sampler reads the writer's data back through the pads. Returns cycles.
size_t pio_check_sequence(ddr3_cycle_t *c, size_t cap);

typedef struct {
    int offset;          // capture word of CK 0's c0, as found from CK
    unsigned pin_errors; // SM cycles where a driven pin differed
    unsigned first_bad;  // SM cycle (from CK 0's c0) of the first, if any
    uint32_t first_want, first_got, first_mask;
    unsigned loop_errors;   // sampled beats that didn't match the written data
    unsigned samples;       // beats checked
} pio_check_result_t;

// Compare a capture (one word per SM cycle, from the synchronised start) and
// the sampler's beats with what the sequence should produce. True if both
// match exactly.
bool pio_check(const ddr3_cycle_t *c, size_t n, const uint32_t *capture, size_t n_capture,
               const bus_sample_t *samples, size_t n_samples, pio_check_result_t *r);

#endif
