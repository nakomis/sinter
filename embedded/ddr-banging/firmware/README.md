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
| `src/dram.c` | Init and whole-burst reads and writes, shared by the shell and the link. Self-refresh between operations. |
| `src/link.c` | ESP32 link, slave side: acts on complete frames of `../proto/ddrlink.h`. Pure logic. |
| `src/link_spi.c` | The link's transport: SPI1 as a mode-3 slave, DMA both ways, CS interrupts, READY. |
| `src/main.c` | The shell, and the link's memory backend. |
| `test/` | Host tests (`make test`). |

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
| `t [addr] [n]` | Write then verify `n` bursts of a pattern. |
| `tck [ns]` | Show or set the clock period (250 to 3900 ns; default 1000). |
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
paying the REFs owed after every burst. That's about 380 KB/s at 1 MHz, so
128 MB takes about 6 minutes each way, plus the SPI time. Each burst still
opens and closes its own row; keeping a row open for several bursts (within
tRAS max) is milestone 11.
