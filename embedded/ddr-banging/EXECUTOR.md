# PIO executor — design note (SINT-100, milestone 11)

The bring-up firmware plays each `ddr3_seq_t` out by **CPU bit-banging**
(`firmware/src/bus_gpio.c`), which tops out at tCK ≈ 250 ns (4 MHz) and
holds interrupts off while it runs. The PIO executor plays the **same
sequences** from three PIO state machines fed by DMA. The sequence builder,
`dram.c`, the ESP32 link and every host test above the bus stay as they are.

Status (2026-10-15):
- **Done, under epio** (nakomis/epio `v0.2.2-nakomis.2`, a submodule):
  - all three SMs: `firmware/src/pio/ddr3_cmd.pio`, `ddr3_wr.pio` and
    `ddr3_smp.pio`;
  - their word streams (`firmware/src/pio_enc.c`);
  - the adapter (`firmware/test/pio_sim.c`).
- **Command SM** (`test/test_pio_cmd.c`): the DRAM sees exactly the
  sequence's C/A on every CK rising edge. There's one edge per cycle, evenly
  spaced, at a 50% duty cycle.
- **All three together** (`test/test_pio_data.c`), at 250 ns, 1 µs and
  3.4 µs:
  - 1 KB written across a row and bank boundary lands in the DRAM model
    byte for byte;
  - read back, it returns byte for byte, with DQS high on every first beat;
  - both pin traces pass the JEDEC checker.
- **Mutations:** a broken CK phase, DQS not rising on a strobe cycle, and a
  sampler loop a cycle long are each caught.
- **Done, firmware side:** `firmware/src/bus_pio.c`, selected with the
  shell's `bus pio`. It builds, but is untested until the board arrives.
  `dram.c` uses an executor interface (`bus.h`); a host test checks it builds
  for, runs on and reports the failures of whichever executor is selected.
  The host tests and the firmware share one clock rule
  (`pioenc_executor_timing`: round down to a whole divider).
- **Next:** the bench checks (§7).

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

Three SMs, one per PIO block. **Instruction memory decided it:** a data SM
that both drives and samples needs about 40 instructions, and a block holds
32.

| SM | Block | GPIOBASE | Pins | Program | Stream |
|---|---|---|---|---|---|
| **cmd** | PIO0 | 16 | OUT 18–40 (C/A), side-set 16–17 (CK, CK#) | `ddr3_cmd.pio`, 7 instr | C/A + 9-bit repeat |
| **writer** | PIO1 | 0 | OUT 0–9 (DQ data; pindirs DQ + DQS), side-set (opt) 8–11 (DQS, DQS#, DIR_DQ, DIR_DQS) | `ddr3_wr.pio`, 28 instr | idle + 16-bit repeat, or drive/strobe + dq[0], dq[1] |
| **sampler** | PIO2 | 0 | IN 0–9 → RX (autopush 10) | `ddr3_smp.pio`, 24 instr | beat-0 / beat-1 flags + 16-bit repeat |

- GPIO 12–15 (RESET#, lane select) and 41–47 stay on SIO and the SPI1 link.
- **Why separate blocks:** PIO maps OUT/side-set within a 32-pin window
  (GPIOBASE 0 or 16), so C/A (16–40) and data (0–11) can't share a block.
  The writer and sampler don't fit one block's memory together either.
- **Sync in time, not by word:**
  - Every path through every program takes exactly **8 SM cycles per CK**
    (2 per quarter), and all three use the same integer divider.
  - One write to PIO0's CTRL enables SM0 of PIO0, PIO1 (NEXT) and PIO2
    (PREV, wrapping) on the same cycle and restarts their dividers.
  - Each stream is therefore run-length encoded independently.
- **Start-up alignment:** the writer and sampler start on an `out pc, 2`
  dispatch (`public start` in each `.pio`). The cmd SM starts on a one-off
  `nop` just before its wrap target. Both then reach CK 0's q0 on the same
  cycle: the first OUT after an empty OSR stalls one cycle in both.

## 3. Word streams (`pio_enc.c`)

Words are consumed LSB first (OSR shifts right):

```
cmd      [22:0]  C/A for GPIO 18..40           [31:23] repeats (0..511)      autopull 32
writer   [1:0]   kind: 0 idle, 1 drive (DQS low), 2 drive + strobe            autopull 18
         idle:   [17:2] repeats (0..65535)
         drive:  [9:2] dq[0]   [17:10] dq[1]   (never repeated)
sampler  [1:0]   bit 0: beat 0 this cycle (q3); bit 1: beat 1 of the previous cycle (q1)
         [17:2]  repeats (0..65535)                                           autopull 18
```

- The writer's and sampler's kind *is* the dispatch address: `out pc, 2`
  into a jump table at offset 0, so both load with `.origin 0`.
- `pioenc_decode` inverts all three and rejects streams that disagree on
  length, a kind 3 writer word, or a beat-1 sample that doesn't follow a
  sampled cycle.

## 4. Timing (`pioenc_timing`, `pio_sim_timing`)

- tCK = 8 × div / 150 MHz, with an integer divider: **53.3 ns × div**. So
  1 µs runs at 1013.3 ns (div 19), 250 ns at 266.7 ns (div 5) and 3.4 µs at
  3413.3 ns (div 64).
- Sequences are built for the tCK actually run, as `bus_set_tck` does now.
- The fastest clock is div 1, 53 ns (18.75 MHz). DLL-off DDR3 allows 8 ns,
  so the translators and wiring will be the limit, not PIO.
- The writer's optional 4-bit side-set leaves no delay bits, so its waits are
  `nop`s. That's why it is 28 instructions.

## 5. Programs

`firmware/src/pio/*.pio`, commented cycle by cycle (c0..c7 of each CK).

- **cmd:** `out pins, 23` / `out x, 9` / `nop` / `jmp x-- rep`, each `[1]`,
  with side-set making CK. A repeat replays the cycle without touching the
  pins.
- **writer:** dispatch to idle, drive or strobe.
  - Drive paths: DIR on (side-set) at c0, then `mov pindirs, ~null` at c1,
    so the translator turns round before the RP2350 drives. dq[0] goes out
    at c2 (q1), DQS rises at c4 (q2) on strobe cycles, dq[1] goes out at c6
    (q3), and the next dispatch is at c7.
  - Idle: `mov pindirs, null` first, then DIR off (opt side-set leaves DIR
    alone until then), then a repeat loop.
- **sampler:** dispatch to none, beat 0, beat 1 or both. `in pins, 10` runs
  at c2 (q1, the previous cycle's beat 1) and c6 (q3, beat 0), with delay
  slots for the waits and repeat loops for long windows.

## 6. Underrun and errors

- If a TX FIFO runs dry, autopull stalls the SM mid-cycle and **CK freezes**.
  epio's MVP models this (side-set is held while stalled), and the JEDEC
  checker should flag it. In practice, DMA from SRAM at about 1.4 M words/s
  won't underrun, but test it: starve the FIFO in the emulator and confirm the
  checker notices.
- Read samples arrive in PIO2's RX FIFO, 2 words per sampled cycle (beat 0
  and beat 1, 10 bits each: DQ, DQS, DQS#), in bus_run's order. The data layout in `dram.c` stays as it
  is: `bus_sample_t[]`, filled by DMA instead of by the CPU.

## 7. Remaining steps

1. ~~epio submodule~~, ~~adapter~~, ~~`.pio` programs~~, ~~tests~~: done.
2. ~~Firmware: `bus_pio.c`~~: done. Selected with `bus pio`; tCK floor 54 ns.
   - Per run: encode, rewind the three SMs, start the DMAs, and wait until
     each TX FIFO is full (the alignment needs it).
   - Then enable all three in sync, and wait until every stream is spent,
     every SM has stalled on its empty FIFO and every sample is in. Then stop.
   - A timeout of twice the expected time is a fault.
3. **On the bench:** a `HARDWARE-CHECKS` pass, comparing the logic-analyser
   capture with the emulator trace for one write run and one read run.
   Check, on silicon:
   - the start-up alignment (§2);
   - that PIO0's PREV mask reaches PIO2 (the RP2350's wrap-round, which epio
     models);
   - that 16-bit DMA reads of the RX FIFO pop one entry each.

## 8. Open questions

- **Input synchroniser:** the sampler currently reads through it (two system
  clocks stale), which is fine at div ≥ 5. Bypass DQ/DQS
  (INPUT_SYNC_BYPASS) at high speed?
- **tDQSCK in DLL-off** (1–10 ns): irrelevant at these clocks. Near 50 ns
  tCK it becomes a sizeable fraction of a quarter; sample on DQS edges
  (`wait pin`) then, not on fixed quarters.
