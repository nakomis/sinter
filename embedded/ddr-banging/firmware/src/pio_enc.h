// Groundwork for the PIO executor (see ../EXECUTOR.md): a ddr3_seq_t turned
// into the two word streams its state machines consume, and the clock
// divider / delay that give a tCK. Pure logic, host-tested.
//
// One word per *run* of identical CK cycles, in both streams at once, so the
// command and data SMs stay in step word for word. Words are consumed LSB
// first (both SMs shift OSR right):
//
//   command  [22:0]  C/A for GPIO 18..40 (CKE, CS0#, CS1#, RAS#, CAS#, WE#,
//                    BA0-2, A0-13); CK/CK# come from side-set
//            [31:23] repeats: further CK cycles with the same C/A (0..511)
//   data     [2:0]   flags: CY_DRIVE, CY_STROBE, CY_SAMPLE
//            [3]     PIOENC_PREV_SAMPLE: the previous cycle sampled, so its
//                    second beat is taken in this cycle's first quarter
//                    (bus_run's order; the SM can't look back a word)
//            [11:4]  dq[0], the beat on DQS rising (0 unless driving)
//            [19:12] dq[1], the beat on DQS falling
//            [22:20] 0
//            [31:23] repeats, equal to the command word's

#ifndef PIO_ENC_H
#define PIO_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddr3.h"

#define PIOENC_CA_BITS    23
#define PIOENC_REP_SHIFT  23
#define PIOENC_REP_MAX    511u
#define PIOENC_PREV_SAMPLE 0x08u

// Encode n cycles. Returns words written to each stream (the same count);
// *overflow if cap ran out (output then holds a prefix of whole runs).
size_t pioenc_encode(const ddr3_cycle_t *c, size_t n, uint32_t *cmd, uint32_t *data,
                     size_t cap, bool *overflow);

// The inverse, for tests and as the executor's reference: returns cycles
// written, or 0 if the streams disagree on a repeat count or cap runs out.
// CK/CK# bits come back as 0 (the executor drives them).
size_t pioenc_decode(const uint32_t *cmd, const uint32_t *data, size_t words,
                     ddr3_cycle_t *out, size_t cap);

// A cycle as the encoder sees it: CK bits and write data that won't be driven
// cleared, so equal cycles really are equal.
ddr3_cycle_t pioenc_normalise(ddr3_cycle_t c);

// PIO timing: each quarter of a CK cycle is `instr` instructions of (1 +
// delay) SM cycles each, an SM cycle being `div` system clocks (integer
// divider). Picks the delay (0..max_delay) and divider closest to tck_ns;
// false if even div = 1, delay 0 is too slow.
typedef struct {
    uint32_t div;
    unsigned delay;
    uint32_t tck_ps;   // what you actually get
} pioenc_timing_t;

bool pioenc_timing(uint32_t tck_ns, uint32_t sysclk_hz, unsigned instr, unsigned max_delay,
                   pioenc_timing_t *out);

#endif
