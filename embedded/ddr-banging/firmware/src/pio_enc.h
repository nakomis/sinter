// The PIO executor's word streams (see ../EXECUTOR.md): a ddr3_seq_t as one
// stream per state machine, and the clock divider for a tCK. Pure logic,
// host-tested.
//
// Three SMs, one per PIO block, started on the same cycle and each taking
// exactly 8 SM cycles per CK, so they stay in step in time. Each stream is
// run-length encoded independently. Words are consumed LSB first (OSR
// shifts right).
//
//   command (PIO0)  [22:0]  C/A for GPIO 18..40 (CKE .. A13); CK/CK# by side-set
//                   [31:23] repeats: further CK cycles with the same C/A (0..511)
//
//   writer  (PIO1)  [1:0]   kind: PIOENC_WR_IDLE, _DRIVE (DQS held low:
//                           preamble/postamble), _STROBE (DQS follows CK)
//                   idle:   [17:2]  repeats (0..65535)
//                   drive:  [9:2]   dq[0], the beat on DQS rising
//                           [17:10] dq[1], the beat on DQS falling
//
//   sampler (PIO2)  [1:0]   PIOENC_SMP_BEAT1: sample the previous cycle's
//                           second beat (q1); PIOENC_SMP_BEAT0: sample this
//                           cycle's first beat (q3)
//                   [17:2]  repeats (0..65535)
//
// Writer and sampler words use 18 bits (autopull threshold 18).

#ifndef PIO_ENC_H
#define PIO_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddr3.h"

#define PIOENC_CA_BITS    23
#define PIOENC_REP_SHIFT  23
#define PIOENC_REP_MAX    511u

#define PIOENC_WR_IDLE    0u
#define PIOENC_WR_DRIVE   1u
#define PIOENC_WR_STROBE  2u
#define PIOENC_SMP_BEAT0  1u
#define PIOENC_SMP_BEAT1  2u
#define PIOENC_WORD_BITS  18          // writer and sampler
#define PIOENC_REP16_MAX  65535u

// Encode n cycles into one SM's stream. Each returns words written; on
// running out of cap, *overflow is set (output holds a prefix of whole runs).
size_t pioenc_cmd(const ddr3_cycle_t *c, size_t n, uint32_t *out, size_t cap, bool *overflow);
size_t pioenc_writer(const ddr3_cycle_t *c, size_t n, uint32_t *out, size_t cap, bool *overflow);
size_t pioenc_sampler(const ddr3_cycle_t *c, size_t n, uint32_t *out, size_t cap, bool *overflow);

// The inverse of all three, for tests and as the executor's reference:
// returns cycles written, or 0 if the streams disagree on length, a word is
// malformed, or cap runs out. CK/CK# bits come back 0 (the cmd SM drives them).
size_t pioenc_decode(const uint32_t *cmd, size_t n_cmd, const uint32_t *wr, size_t n_wr,
                     const uint32_t *smp, size_t n_smp, ddr3_cycle_t *out, size_t cap);

// A cycle as the encoders see it: CK bits, undriven write data and STROBE
// without DRIVE cleared, so equal cycles really are equal.
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

// The executor's own rule (8 SM cycles per CK): the largest integer divider
// whose tCK is at most tck_ns (never slower than asked: the refresh cap is a
// maximum), at least 1. Fills div and tck_ps (delay 0). Shared by bus_pio.c
// and the host tests so both run the same clock.
void pioenc_executor_timing(uint32_t tck_ns, uint32_t sysclk_hz, pioenc_timing_t *out);

#endif
