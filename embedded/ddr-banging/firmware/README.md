# DDR-banging firmware (SINT-100)

RP2350B bring-up firmware for driving one x8 chip of a DDR3 UDIMM: SPD read,
DLL-off initialisation, and single 8-byte bursts, driven from a USB serial
shell. Target board: Waveshare Core2350B. Pin map: the channel board's
(`../channel-board/DESIGN.md` §2), plus RESET# on GPIO 15 (spike only).

## Build

Needs pico-sdk 2.2.0 and the Arm GNU toolchain (paths are the Makefile
defaults; override with `PICO_SDK_PATH` / `PICO_TOOLCHAIN_PATH`).

```sh
make test    # host unit tests: command encoding, timing, refresh, SPD decoding
make         # build/ddr_banging.uf2
make flash   # picotool load (hold BOOTSEL while plugging in the first time)
```

## Layout

| File | What |
|---|---|
| `src/ddr3.c` | Command encoding, mode registers, address split, and the cycle-by-cycle sequence builder (init, read, write, self-refresh, auto-inserted REFs). Pure logic. |
| `src/spd.c` | DDR3 SPD decode and CRC. Pure logic. |
| `src/bus_gpio.c` | Plays a sequence out by bit-banging, CPU-timed in quarter clocks. Interrupts off while it runs. |
| `src/main.c` | The shell. |
| `test/` | Host tests (`make test`). |

A sequence is a list of CK cycles, each with a command/address word and data
flags (drive, strobe, sample). The executor is deliberately dumb so a PIO
executor can replace it later and play the same sequences.

## Shell

Connect with `screen /dev/tty.usbmodem* 115200` (or any serial terminal).

| Command | What |
|---|---|
| `spd` | Read the SPD EEPROM over I2C and decode it. Proves the fixture. |
| `init` | RESET#, then DLL-off init (MR2, MR3, MR1, MR0, ZQCL), then self-refresh. |
| `w <addr> <hex16>` | Write one 8-byte burst. |
| `r <addr>` | Read one burst; prints the whole sample window with DQS levels. |
| `t [addr] [n]` | Write then verify `n` bursts of a pattern. |
| `tck [ns]` | Show or set the clock period (250 to 3900 ns; default 1000). |
| `hold <line>` / `hold off` | One line high, everything else low, for continuity checks. |
| `walk [ms]` | Step `hold` through every line. |

## Wiring notes (spike)

- **DM0 must be tied to VSS** on the fixture's header: the firmware never
  drives it (low = write every byte).
- ODT0, CKE1, S1#, SA0-2 are tied on the breakout boards. CS1# is driven
  high anyway.
- GPIO 12-14 (the channel board's lane decoder) are held low, selecting
  lane 0; unused on the spike. On the channel board RESET# comes from the I/O
  expander rather than GPIO 15, so `bus_reset()` will need porting there.

## Timing choices

- **DLL off**, CL = CWL = 6, BL8, ODT off. Read data arrives CL - 1 clocks
  after READ. The read window samples a cycle either side, so a wrong
  assumption shows up as data shifted in the window rather than garbage.
- **Clock 1 MHz by default.** DLL-off needs tCK ≥ 8 ns, and Micron's 1 Gb
  sheet caps it at 7.8 µs (3.9 µs above 85 °C). Above ~3.9 µs the REFs can't
  keep pace and an open row exceeds tRAS(max), so the shell allows 250 ns to
  3.9 µs. Near 250 ns the bit-bang loop is the limit; `tck` reports the
  measured period.
- **Refresh:** at 1 MHz a REF is due every 7 clocks. The sequence builder
  inserts them whenever every bank is closed, and pays back REFs postponed
  while a row was open (up to 8 may be postponed), paying them all off before
  entering self-refresh.
- **Between commands the DRAM is in self-refresh**, so the clock can stop.
  Each command exits self-refresh (waiting 512 clocks, refreshing), does its
  work, and re-enters.
- MR0 DLL reset is skipped, as is tDLLK. If reads misbehave, try adding them
  (see `../datasheet-notes.md`).
