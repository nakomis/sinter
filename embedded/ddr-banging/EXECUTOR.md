# PIO executor — design note (SINT-100, milestone 11)

The bring-up firmware plays each `ddr3_seq_t` out by **CPU bit-banging**
(`firmware/src/bus_gpio.c`), which tops out at tCK ≈ 250 ns (4 MHz) and
holds interrupts off while it runs. The PIO executor plays the **same
sequences** from two PIO state machines fed by DMA. The sequence builder,
`dram.c`, the ESP32 link and every host test above the bus stay as they are.

Status (2026-10-15):
- **Done:** the word streams and timing maths (`firmware/src/pio_enc.c`).
- **Done:** the **command SM**, `firmware/src/pio/ddr3_cmd.pio`, runs under
  epio (nakomis/epio `v0.2.2-nakomis.1`, a submodule). For init and run
  batches at 250 ns, 1 µs and 3.4 µs, the DRAM sees exactly the sequence's
  C/A on every CK rising edge. There is one edge per cycle, evenly spaced,
  and the duty cycle is exactly 50%. The pin trace passes the JEDEC checker,
  which now also checks tCH/tCL. A deliberately broken CK phase is caught.
- **Next:** the data SM (§5), then `bus_pio.c` on the firmware side.

## 1. The spec: bus_run's pin trace

`bus_run` already defines exactly what the pins do in each quarter of a CK
cycle, and the JEDEC checker (`test/jedec.c`) and DRAM model
(`test/dram_model.c`) prove that trace correct. **The PIO executor's job is
to reproduce it.** Scaling time is allowed; any other difference fails a test.

| Quarter | CK | Command pins (16–40) | Data pins (0–11) |
|---|---|---|---|
| q0 | falls | C/A for this cycle | DQ drive on/off (DIR + pindirs); DQS low / DQS# high if driving |
| q1 | low | — | DQ = dq[0] if driving; sample beat 1 of the previous cycle if it sampled |
| q2 | rises (the DRAM registers C/A) | — | DQS high / DQS# low if strobing |
| q3 | high | — | DQ = dq[1] if driving; sample beat 0 if sampling |

## 2. Layout

| SM | Block | GPIOBASE | OUT | Side-set | IN | Fed by |
|---|---|---|---|---|---|---|
| **cmd** | PIO0 | 16 | 18–40 (23 pins: CKE … A13) | 16–17 (CK, CK#) | — | DMA ← command stream |
| **data** | PIO1 | 0 | 0–7 (DQ), pindirs 0–9 | 8–11 (DQS, DQS#, DIR_DQ, DIR_DQS) | 0–9 → RX FIFO | DMA ← data stream; DMA → samples |

- GPIO 12–15 (RESET#, lane select) and 41–47 stay on SIO and the SPI1 link,
  as now. PIO2 is free; it could later host a PIO SPI/QSPI slave for the
  ESP32 link.
- **Why two blocks:** a PIO block maps OUT/side-set within a 32-pin window
  (GPIOBASE 0 or 16). C/A needs 16–40 and data needs 0–11, so no single
  window covers both.
- **Sync:** both SMs start on the same system clock, using CTRL's
  NEXTPREV_SM_ENABLE and NEXTPREV_CLKDIV_RESTART. They stay in step because
  every path through each program takes **the same number of SM cycles per
  CK**, with the same divider and delay. A cross-block `irq next` / `wait irq
  prev` handshake at sequence start (and optionally every N words) guards
  against drift. epio models all of this exactly.

## 3. Word streams (done: `pio_enc.c`)

One word per run of identical CK cycles, in **both** streams at once, so word
k of each describes the same cycles. LSB first (OSR shifts right).

```
command  [22:0]  C/A for GPIO 18..40          [31:23] repeats (0..511)
data     [2:0]   DRIVE, STROBE, SAMPLE
         [3]     PREV_SAMPLE (beat 1 of the previous cycle is due in q1)
         [11:4]  dq[0]   [19:12] dq[1]   [22:20] 0   [31:23] repeats
```

- `pioenc_decode` inverts it exactly. `test/test_pio_enc.c` round-trips init,
  write-run and read-run batches at 250 ns, 1 µs and 3.4 µs, and runs the
  JEDEC checker on the decoded form.
- **Compression:** init sequences shrink about 33× (512 CK of tZQinit become
  one word). Run batches shrink 2.5–2.9×, because write data changes every
  cycle.
- **DMA rate:** at tCK 250 ns that's roughly 1.4 M words/s per stream, which
  DMA handles easily.
- **Sizing:** a 2 KB batch is 580–1,800 words per stream at 4 bytes each, so
  two 7 KB buffers, or one per stream in a ring.

## 4. Timing (done: `pioenc_timing`)

Each quarter is `instr` instructions × (1 + delay) SM cycles × div system
clocks, with an **integer** divider. Integer dividers avoid the fractional
divider's jitter, and epio's MVP models integer only.

- At 150 MHz, tCK = 4 × instr × (1 + delay) × div / 150 MHz.
- **Exact** at 2 µs, and at every tCK that's a multiple of 26.7 ns × instr.
  1 µs comes out at 986.7 or 1013.3 ns, which is fine: the shell reports the
  real tCK, and the refresh maths uses whatever tCK it's given.
- **Delay bits:** the data SM's 4 side-set pins leave **1 delay bit** (0–1),
  and the cmd SM's 2 leave 3 (0–7). Both SMs must use identical timing, so use
  `max_delay = 1` for both and let the divider do the work.
- **The fastest clock** is set by the longest quarter: whichever SM needs the
  most instructions in one quarter. With 2 instructions per quarter at div 1,
  that's 8 SM cycles per CK = 53 ns (18.75 MHz). With 1 instruction it's
  26.7 ns. The DLL-off floor is 8 ns, so PIO is never the limit DDR3 imposes.
  The real limits are the translators and the wiring.

## 5. Draft programs (to be written against epio)

**cmd** (1 instruction per quarter; side-set bit 0 = CK on GPIO 16, bit 1 =
CK# on 17). This is the version in `src/pio/ddr3_cmd.pio`, tested:

```
.program ddr_cmd
.side_set 2
.wrap_target
top:
    out pins, 23    side 0b10        ; q0: CK low, CK# high; C/A out (autopull at 32)
    out x, 9        side 0b10        ; q1: repeats
loop:
    nop             side 0b01        ; q2: CK rises: the DRAM registers C/A
    jmp x-- rep     side 0b01        ; q3: repeats left? (tests, then decrements)
.wrap                                ;     none: wrap to top for the next word
rep:
    nop             side 0b10        ; q0: same C/A, still on the pins
    jmp loop        side 0b10        ; q1
```

Six instructions. Every CK is exactly 4 instructions on both paths
(top → loop, and rep → loop). If the FIFO is empty, `out pins` stalls in q0
with CK held low. That's a legal clock stretch for DDR3 only in
self-refresh, which is why every sequence ends with SRE before the stream
runs out.

**data** (dispatch per word on its flags; 2 instructions per quarter is
probably needed):

- `out pc, 4` on the flags would dispatch straight to one of up to 16
  handlers, but there are only 32 words of instruction memory per block, so
  the handlers must be tiny.
- **Kinds that occur:**
  - idle;
  - sample;
  - prev-sample only;
  - sample + prev-sample;
  - drive (preamble/postamble);
  - drive + strobe.
- That's six kinds × 4 quarters. Each handler also needs its own repeat loop.

**Alternative:** split data into two SMs in PIO1. A **writer** (DRIVE,
STROBE, DQ) and a **sampler** (SAMPLE only, `in pins, 10` at q1/q3) would
each be small. The sampler could even take its own simpler stream, of
"sample for n cycles starting at cycle k" records.

Decide with the emulator. Instruction memory, not speed, is the constraint.

## 6. Underrun and errors

- If a TX FIFO runs dry, autopull stalls the SM mid-cycle and **CK freezes**.
  epio's MVP models this (side-set is held while stalled), and the JEDEC
  checker should flag it. In practice, DMA from SRAM at about 1.4 M words/s
  won't underrun, but test it: starve the FIFO in the emulator and confirm the
  checker notices.
- Read samples arrive in the RX FIFO, 2 words per sampled cycle (beat 0 and
  beat 1, 10 bits each: DQ, DQS, DQS#). The data layout in `dram.c` stays as it
  is: `bus_sample_t[]`, filled by DMA instead of by the CPU.

## 7. Plan once epio is tagged

1. Add epio as a git submodule at `firmware/test/third_party/epio`, pinned to
   the tag.
2. **Adapter** (`test/pio_sim.c`): epio's per-tick hook → `sim_out`/`sim_oe` +
   `sim_record()`; `sim_input` → epio GPIO input levels. Ticks → sim time
   (one sim unit is a quarter clock today, so scale).
3. **`.pio` programs** in `firmware/src/`, assembled by pioasm at build time
   (`pico_generate_pio_header` on the firmware side; host tests load the same
   `-o c-sdk` header into epio).
4. **Tests:** for each batch shape and tCK, compare the PIO-played pin trace
   with bus_run's (time-scaled), check it with `jedec_check_pins`, and run
   the DRAM model round trip.
5. **Firmware:** `bus_pio.c` with the same `bus_run` signature, selected at
   build time or by `tck` (PIO below 250 ns, bit-bang above, or PIO always).
   Then the shell's tCK floor drops.
6. **On the bench:** a `HARDWARE-CHECKS` pass. Compare the logic-analyser
   capture with the emulator trace for one write run and one read run.

## 8. Open questions

- One data SM or two? (§5)
- Is IN read-back of driven pins exact in epio? The pio-emulator session is
  testing this rather than assuming.
- Should the sampler use the 2-cycle input synchroniser, or bypass it on
  DQ/DQS? Bypassing makes sampling exactly the q1/q3 instant. With the
  synchroniser, samples are two SM cycles stale, which is fine at low
  speeds but must be accounted for at speed.
- tDQSCK in DLL-off (1–10 ns) is irrelevant at bit-bang speeds. At ≥ 20 MHz
  it's a sizeable fraction of a quarter: worth sampling on DQS edges then
  (`wait pin`), not on fixed quarters.
