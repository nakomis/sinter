# DDR-banging firmware (SINT-100)

RP2350B bring-up firmware for driving one x8 chip of a DDR3 UDIMM: SPD read,
DLL-off initialisation, and single 8-byte bursts, driven from a USB serial
shell. Target board: Waveshare Core2350B. Pin map: the channel board's
(`../channel-board/DESIGN.md` §2), plus RESET# on GPIO 12 (spike only: the
channel board's lane decoder pin, which the spike doesn't have). The ESP32
link is SPI1 on GPIO 44-46 plus TX on 15, never 47 (the Core2350B's PSRAM CSn).

## Build

Needs pico-sdk 2.2.0 and the Arm GNU toolchain (paths are the Makefile
defaults; override with `PICO_SDK_PATH` / `PICO_TOOLCHAIN_PATH`).

```sh
git submodule update --init   # once: epio + apio, the host PIO emulator
make test    # host unit tests: command encoding, timing, refresh, SPD decoding, PIO
make         # build/ddr_banging.uf2
make flash   # picotool load (hold BOOTSEL while plugging in the first time)
```

## Layout

| File | What |
|---|---|
| `src/ddr3.c` | Command encoding, mode registers, address split, and the cycle-by-cycle sequence builder (init, read, write, self-refresh, auto-inserted REFs). Pure logic. |
| `src/spd.c` | DDR3 SPD decode and CRC. Pure logic. |
| `src/bus_gpio.c` | Plays a sequence out by bit-banging, CPU-timed in quarter clocks. Interrupts off while it runs. |
| `src/pio_enc.c` | The PIO executor's word streams (`../EXECUTOR.md`): a sequence as command and data words, one per run of identical cycles, and the PIO clock divider for a tCK. Pure logic. |
| `src/dram.c` | Init and whole-burst reads and writes, shared by the shell and the link. Self-refresh between operations. |
| `src/link.c` | ESP32 link, slave side: acts on complete frames of `../proto/ddrlink.h`. Pure logic. |
| `src/link_spi.c` | The link's transport: SPI1 as a mode-3 slave, DMA both ways, CS interrupts, READY. |
| `src/main.c` | The shell, and the link's memory backend. |
| `src/pio/` | PIO programs: `ddr3_cmd.pio` (command/address + CK) so far. Assembled by pioasm, which the host tests build from the pico-sdk. |
| `test/third_party/` | Submodules: nakomis/epio (host RP2350 PIO emulator, tag `v0.2.2-nakomis.1`) and apio v0.3.0, its headers. |
| `test/pio_sim.c` | Runs PIO programs under epio, fed like DMA, recording the pins into the same log the JEDEC checker and DRAM model read. |
| `test/` | Host tests (`make test`). `test/jedec.c` is an independent JEDEC timing checker: every sequence the firmware builds, at every clock it allows, read back off the simulated pins. `test/dram_model.c` is a behavioural DRAM on the same pins: it stores writes at their DQS edges and drives reads at RL, so `dram_write`/`dram_read` are tested byte for byte. |

A sequence is a list of CK cycles, each with a command/address word and data
flags (drive, strobe, sample). The executor is deliberately dumb so a PIO
executor can replace it later and play the same sequences.

## Shell

Connect with `screen /dev/tty.usbmodem* 115200` (or any serial terminal).

| Command | What |
|---|---|
| `spd` | Read the SPD EEPROM over I2C and decode it. Proves the fixture. |
| `temp` | Module temperature from the on-DIMM thermal sensor (0x18). |
| `init` | RESET#, then DLL-off init (MR2, MR3, MR1, MR0, ZQCL), then self-refresh. |
| `w <addr> <hex16>` | Write one 8-byte burst. |
| `r <addr>` | Read one burst; prints the whole sample window with DQS levels. |
| `t [addr] [n]` | Write then verify `n` bursts of a pattern, and report KB/s. |
| `v [addr] [n]` | Verify only: read back what an earlier `t` wrote (soak tests). |
| `tck [ns]` | Show or set the clock period (250 to 3400 ns; default 1000). |
| `hold <line>` / `hold off` | One line high, everything else low, for continuity checks. |
| `walk [ms]` | Step `hold` through every line. |
| `link` | ESP32 link statistics: command frames, errors, last error. |

## Wiring notes (spike)

- **DM0 must be tied to VSS** on the fixture's header: the firmware never
  drives it (low = write every byte).
- ODT0, CKE1, S1#, SA0-2 are tied on the breakout boards. CS1# is driven
  high anyway.
- GPIO 12-14 (the channel board's lane decoder) are held low, selecting
  lane 0; unused on the spike, where GPIO 12 is RESET# instead. On the channel
  board RESET# comes from the I/O expander, so `bus_reset()` will need porting
  there.

## Timing choices

- **DLL off**, CL = CWL = 6, BL8, ODT off. Read data arrives CL - 1 clocks
  after READ. The read window samples a cycle either side, so a wrong
  assumption shows up as data shifted in the window rather than garbage.
- **Clock 1 MHz by default.** DLL-off needs tCK ≥ 8 ns, and Samsung sets no
  maximum beyond meeting refresh. The firmware's limit is refresh: no REF can
  go in while a row is open, and a write holds its row until the next REF for
  up to 18 clocks, while JEDEC allows only 8 REFs postponed. 18 × tCK ≤
  8 × tREFI gives the shell's 3.4 µs cap; `test/jedec.c` checks it. Near
  250 ns the bit-bang loop is the limit; `tck` reports the measured period.
- **Refresh:** at 1 MHz a REF is due every 7 clocks. The sequence builder
  inserts them whenever every bank is closed, and pays back REFs postponed
  while a row was open (up to 8 may be postponed), paying them all off before
  entering self-refresh.
- **Temperature:** above 85 °C Tcase, tREFI halves to 3.9 µs, and self-refresh
  only holds data with MR2's SRT bit set, which is only allowed for chips with
  the extended range (SPD byte 31). `init` reads the SPD and sets SRT when it
  may. Every DRAM operation reads the on-module sensor first. From 80 °C
  (margin for sensor accuracy and case offset), the firmware switches to the
  2× schedule and caps tCK at 1.7 µs; below 78 °C it switches back. A stick
  that is hot but lacks the extended range gets an error instead. No sensor:
  assumed cool.
- **Between commands the DRAM is in self-refresh**, so the clock can stop.
  Each command exits self-refresh (waiting 512 clocks, refreshing), does its
  work, and re-enters.
- MR0 DLL reset is skipped, as is tDLLK. If reads misbehave, try adding them
  (see `../datasheet-notes.md`).

## ESP32 link (milestone 10)

Protocol: `../proto/ddrlink.h`. Client: `../esp32-client/`. While the shell
runs a command, READY stays low and the ESP32 waits.

| GPIO | Signal | ESP32-S3 (test app) |
|---|---|---|
| 44 | SPI1 RX ← MOSI | 11 |
| 45 | SPI1 CSn ← CS | 10 |
| 46 | SPI1 SCK ← SCK | 12 |
| 15 | SPI1 TX → MISO | 13 |
| 41 | READY → | 9 |

Both sides are 3.3 V: wire them directly, with a common GND. SPI mode 3, at up
to ~12 MHz (the RP2350 slave needs clk_peri ≥ 12 × SCK).

Reads and writes share one self-refresh exit between up to 256 bursts (2 KB),
in runs of consecutive bursts per open row: one ACT and one PRE per run, a
column command every 4 CK, data seamless. No REF can go in while a row is
open, so a run is as long as JEDEC's 8 postponed REFs allow
(`ddr3_seq_row_bursts`): 10 bursts at 1 MHz, 58 at 250 ns, 1 at 3.4 µs.

| tCK | Write | Read |
|---|---|---|
| 250 ns | 4.8 MB/s | 4.8 MB/s |
| 1 µs (default) | 940 KB/s | 970 KB/s |
| 2 µs | 220 KB/s | 240 KB/s |
| 3.4 µs | 60 KB/s | 67 KB/s |

Memory side only; the SPI link at 8 MHz carries about 1 MB/s. At 1 MHz the
128 MB test takes a few minutes each way.
