# Bring-up runbook: parcel to first burst (SINT-100)

Each step lists what to do, what "good" looks like, and what to try when it
isn't good. Steps follow PLAN.md's milestones (M2–M9). Do them in order: each
step only uses the parts the steps before it have proved.

Sticks: **reference** = the stock M391B5673FH0, never modified. **Sacrificial**
= the one that will be pogo-marked and have its termination removed.

Meter modes: **Ω** = resistance; **diode** = diode test, which reads the
forward voltage in volts.

---

## Step 0 — before the parcels (multimeter and a stick only)

Two of PLAN.md's open questions can be answered today. Both measurements also
give a baseline for checking the fixture in step 4.

### 0a. Termination: values and clock

Use the **reference** stick, unpowered, on an anti-static mat. Probe the gold
fingers directly. Pin numbers are in `pogo-pins.md`, and the pin-1 end is
marked on the stick's label side.

| Measure | Between | Expect | Tells us |
|---|---|---|---|
| R_term | VTT (120) ↔ BA2 (52), A0 (188), RAS# (192), CKE0 (50) | ~30–60 Ω, the same on every line | Which lines have termination to VTT, and its value |
| VTT ↔ VSS | 120 ↔ 2 | Open, or a slowly rising reading (decoupling capacitors charging) | VTT isn't shorted to ground |
| CK pair | CK0 (184) ↔ CK0# (185) | ~100–200 Ω if terminated differentially; open if AC (RC) terminated | The "how is the clock terminated?" question |
| CK to VTT | 184 ↔ 120 | Open if the clock isn't terminated to VTT | The same question |
| DQ | DQ0 (3) ↔ VTT | Open | DQ isn't terminated on the module (expected; ODT does it) |

Write the numbers into PLAN.md's open questions.

**If R_term is about 40 Ω:** a 1.5 V high pulls ~19 mA into VTT, more than an
AXC output can source at 1.5 V. That confirms step 6 (remove the termination)
is needed before any C/A line is driven with the stick powered.

**If CK0↔CK0# is about 100 Ω:** driving CK high and CK# low pushes
~15 mA through two translator outputs. That is marginal. Look at the CK edges
in step 9, and consider removing that resistor in step 6 too.

### 0b. Diode baseline of every wired pad

Still the **reference** stick, unpowered. With the meter on diode test, put
**red on VSS (pin 2)** and **black on the pad**. Each DRAM input has an ESD
diode to VSS, so a pad that reaches a chip reads roughly **0.4–0.8 V**. Record
every pad in `pogo-pins.md`, other than VDD/VSS/VTT/VREF, as a table:
`pin, signal, volts`.

- Signals on all 18 chips (C/A, CK) read lower than DQ, which reaches only
  two chips (a rank-0 chip and its rank-1 twin): more diodes in parallel.
- **OL (open) on a C/A pad** means the termination resistor path is hiding it,
  or the meter's test current is too low. Note it and move on.
- **0.00 V** means a short to VSS. Expected on none of the signal pads.

This table is the reference for step 4: once the sacrificial stick is in the
fixture, the same readings taken at the header pins prove every pogo pin lands.

### 0c. Print the fixture test strip

When the regular resin is in the Mars 5 Ultra: `fixture/stl/test.stl`. Push a
P75 into each hole size and pick the one that holds without cracking. Set the
fixture's hole size to match before printing the jaws.

---

## Step 1 — Core2350B on its own (no wiring)

1. Plug the Core2350B into USB through the FFC adapter, holding **BOOTSEL**.
   An `RP2350` drive should appear.
2. In `firmware/`, run `make test`. The host tests should all pass. Then run
   `make flash`.
3. Run `screen /dev/tty.usbmodem* 115200`. Expect
   `ddr-banging (SINT-100). 'help' for commands.`
4. Run `tck`. Expect `tCK 1000 ns (1.00 MHz)` or close to it; the period is
   measured, not assumed. Try `tck 250`, then `tck 1000` to put it back.
5. Run `spd`. Expect `no answer from the SPD at 0x50` (nothing is attached
   yet), with no hang.

**If something goes wrong:**
- **No drive appears:** try another cable or port. On A2 silicon, BOOTSEL has
  to be held while the board powers up, not just pressed.
- **No serial port after flashing:** the board may still be in BOOTSEL. Unplug
  it and plug it back in *without* the button held.
- **`spd` hangs:** the I2C pins have no pull-ups and the bus is stuck. See the
  pull-up note in step 5. It shouldn't happen with nothing attached.

---

## Step 2 — translator check (M3), one chip at a time

Fakes and duds fail obviously, so test **every chip** before soldering it into
the build. It takes about a minute each.

SN74AXC8T245 pinout (TSSOP-24 on the adapter's 0.65 mm side):

| Pin | | Pin | |
|---|---|---|---|
| 1 | VCCA (3.3 V) | 24 | VCCB (1.5 V) |
| 2 | DIR (high = A→B) | 23 | VCCB |
| 3–10 | A1–A8 | 22 | OE# (low = enabled) |
| 11–13 | GND | 21–14 | B1–B8 |

Check this pinout against TI's SN74AXC8T245 datasheet once, on
the first chip, before trusting it for the rest.

Use the Core2350B's 3V3 pin for VCCA. Take VCCB from the bench supply at
**1.5 V, limited to 50 mA**. Fit 100 nF on each rail. Tie OE# to GND.

| Test | Set up | Expect |
|---|---|---|
| Idle current | Both rails on, DIR high, all A inputs tied to GND | VCCB draws < 1 mA (the bench supply reads ~0) |
| A→B high | DIR high, A1 to 3.3 V | B1 = 1.45–1.5 V |
| A→B low | DIR high, A1 to GND | B1 < 0.05 V |
| Every channel | Repeat for A2–A8 | The same |
| B→A | DIR low, B1 to 1.5 V, then to GND | A1 = ~3.3 V, then ~0 V |
| Disabled | OE# high | B outputs float (the reading wanders, nothing solid) |

- **A fake** usually draws tens of mA, or outputs track the wrong rail. For
  example, B reads 3.3 V because it isn't really a dual-supply part, which
  would kill the DRAM later. Bin it.
- **One dead channel:** keep the chip for DQS (that board uses only 2 of its
  8 channels) and note which channel is dead.
- **No input left floating:** the AXC has no bus-hold, so a floating input
  oscillates and draws current. That's why the A inputs are tied to GND in
  the idle test.

---

## Step 3 — debug setup (M4), optional for the early steps

The early steps (SPD, `walk`) need only the meter. Set this up before step 9,
because `init` is when you need to see the bus.

- **Logic analyser:** sigrok-pico on the RP2040, into PulseView. It is
  **3.3 V only**, so probe the **A sides** (RP2350 GPIOs), never the 1.5 V
  side. Every DRAM signal has a 3.3 V copy on the GPIO side; on reads, that
  copy is the translator's B→A output.
  - Channels, in priority order: CK (16), CKE (18), CS0# (19), RAS# (21),
    CAS# (22), WE# (23), DQS (8), DIR_DQ (10). Add DQ0 (0) if channels are
    spare.
  - At 1 MHz CK, sample at ≥ 10 MHz.
- **SWD:** debugprobe firmware on the RP2040 (or a Pico 2), wired to the
  Core2350B's SWCLK/SWDIO pads. You only need this if the firmware crashes;
  `printf` over USB covers everything else.

---

## Step 4 — fixture assembly and continuity (M2)

1. Solder the P75s into the **bare** PCBs first, then fit the resin jaw
   (fixture/pcb notes). Solder on the outer side, and keep the tips at
   mid-stroke height.
2. Bolt up PCB–jaw–floor–jaw–PCB with no stick. On each board, check:
   - every header pin to its pogo tip: < 1 Ω;
   - all VSS header pins to each other: < 1 Ω;
   - VDD to VSS: open;
   - VREF to VSS: open now, ~500 Ω once the divider's resistors are fitted.
3. Seat the **sacrificial** stick. Locate it on the key notch and the board
   edge, and tighten the bolts evenly.
4. Repeat step 0b at the **header pins** instead of the gold fingers: red on
   a VSS header pin, black on each signal header pin. Each reading should
   match the step-0b table to within ~0.05 V. Also check:
   - VSS to VSS across the two jaws (front J1 pin 1 ↔ back J1 pin 1): < 1 Ω
     through the stick's ground plane;
   - VDD header ↔ VDD header across the jaws: < 1 Ω, likewise through the
     stick.

**If a reading is wrong:**
- **OL on one signal:** that pogo pin isn't touching its pad. Reseat the
  stick. If it's always the same pin, the pin is bent or set too short.
- **0 V between two neighbouring signals:** a pogo pin is bridging two pads.
  The stick is offset along its length; check the end stops.
- **A whole row reads OL:** the clamp is too loose on that face, or the row
  height (0.75 / 2.03 mm) is off. Check against the insertion gauge.

---

## Step 5 — read the SPD (M5): 3.3 V only, no 1.5 V yet

The SPD EEPROM and the thermal sensor run from VDDSPD (3.3 V), so this step
needs **no DRAM power at all**. It is the safest first contact with the
stick.

Wiring (headers: front J1 = `VSS DQ0 DQ1 DQ2 DQ3 DQS0 DQS0# SCL VTT VREF VDD
CKE0`, back J1 = `VSS DQ4 DQ5 DQ6 DQ7 DM0 CK0 CK0# SDA VDDSPD VTT VDD RESET#`):

| Header | To |
|---|---|
| back VDDSPD | Core2350B 3V3 |
| front SCL | GPIO 43 |
| back SDA | GPIO 42 |
| both VSS | Core2350B GND |
| SDA, SCL | **4.7 kΩ pull-up each to 3V3.** The internal pull-ups are too weak for the stick's bus capacitance. |

Leave VDD, VTT and every other header unconnected.

Run `spd`. Expect a 16×16 hex dump, then:

```
DDR3: yes  CRC: ok (...)
part 'M391B5673FH0-CH9', type ..., 2048 MB, 2 rank(s), x8 chips of 1024 Mb, 8 banks
rows 14 bits, cols 10 bits, bus 64+ECC, 1.5 V, tCKmin 1500 ps, tAAmin 13500 ps, CL mask 003e
extended temperature: ...; auto self-refresh: ...; thermal sensor: yes
```

Then run `temp`. Expect `module 2x.xx C`, about room temperature. Put a warm
finger on the sensor chip (the small one in the middle of the label side) and
the reading should rise.

**If it fails:**
- **`no answer from the SPD`:**
  - check SDA/SCL aren't swapped, that VDDSPD reads 3.3 V at the header, and
    that the pull-ups are fitted;
  - SA0–SA2 are tied to VSS on the boards (address 0x50): check those pins
    read 0 Ω to VSS;
  - scope SCL: if it never goes low, it's the GPIO wiring; if it never goes
    high, a pull-up is missing.
- **`CRC: BAD`:** a marginal bus, giving random bit errors. Run it three
  times: if the dump changes, slow down (drop `i2c_init` in `main.c`
  to 50 kHz, or fit stronger pull-ups, 2.2 kΩ). If it's stable but still BAD, this is a decode
  bug: save the dump and compare it with `test/test_spd.c`.
- **`no thermal sensor`:** some HP-branded sticks don't fit it. That isn't
  fatal: refresh then assumes ≤ 85 °C.

*Milestone: the fixture is proven, because 4 pogo pins carried I2C to a chip
under the clamp.*

---

## Step 6 — remove the termination (M6), sacrificial stick only

Do this only if step 0a found ~40 Ω to VTT, which it will on a UDIMM.

1. With the meter, find the resistor arrays: one end on VTT (pin 120/240),
   the other on a C/A line. On ECC UDIMMs they are 4- or 8-resistor 0402
   arrays (marked `RN`), usually at the end of the fly-by. Trust the meter, not the location.
2. Use hot air at ~350 °C, a small nozzle, and Kapton over the neighbouring
   DRAM. Lift each array. Leave the VTT decoupling capacitors in place;
   they're harmless.
3. If step 0a showed CK0↔CK0# ≈ 100 Ω and you want it gone, it's a single
   0402 between the two CK traces near the first chip. This is optional:
   decide after looking at CK in step 9.
4. Re-measure: VTT ↔ every C/A pad should now be **open**. Re-take the step-0b
   diode readings. They should be unchanged (or clearer, where OL readings
   were hidden before).

**If it goes wrong:**
- **A C/A pad now reads OL in diode mode:** a pad was lifted with the array.
  Inspect it under magnification, and bridge with a 0.1 mm wire to the trace.
- **The stick is ruined:** it was the sacrifice. There's a second
  K4B1G0846F stick, and the other sticks in the drawer will do in a pinch.

---

## Step 7 — wire the translators, check with `walk` before 1.5 V touches the stick

Build it all **with the stick out of the fixture**.

| Translator | Channels (A side from GPIO, B side to header) | DIR | OE# |
|---|---|---|---|
| U1 | CK 16, CK# 17, CKE 18, CS0# 19, RAS# 21, CAS# 22, WE# 23, RESET# 12 | 3.3 V (A→B) | GND |
| U2 | BA0 24, BA1 25, BA2 26, A0 27, A1 28, A2 29, A3 30, A4 31 | 3.3 V | GND |
| U3 | A5 32 … A12 39 | 3.3 V | GND |
| U4 | A13 40; channels 2–8 spare | 3.3 V | GND |
| U5 | DQ0–DQ7, GPIO 0–7 | GPIO 10 | GND |
| U6 | DQS 8, DQS# 9; channels 3–8 spare | GPIO 11 | GND |

- **Spare channels:** tie **both** A and B to GND on U4–U6. On U6 the direction
  flips, so either side may be the input.
- **CS0#** goes to the back header's `S0#`. GPIO 20 (CS1#) is left
  unconnected, because S1# is tied high on the board.
- **Strap headers:** **DM0 → VSS** (the firmware never drives it) and
  **ODT0 → VSS** (ODT stays off).
- **RESET# pull-down:** a 10 kΩ from GPIO 12 / U1's A input to GND, so RESET#
  stays low while the RP2350 boots and its GPIOs float.
- **Optional:** 47 kΩ from each U5/U6 B pin to VSS. These stop the DQ inputs
  floating when the DRAM isn't driving (between reads). Fit them if the step-9
  read windows show noise outside the data beats.
- **Rails:** VCCA from 3V3, VCCB and the DIMM VDD from the **same** 1.5 V bench
  output. VREF comes from the front board's divider (VDD/2), and VTT is left
  unconnected. Fit 100 nF per translator per rail.

With the 1.5 V bench supply on (limit 100 mA), no stick fitted, and the
firmware running:

1. Run `walk 3000`. At each step, put the meter on the **header pin** of the
   named line: 1.5 V while it's named, ~0 V otherwise. Spot-check a neighbour
   or two each time to catch swapped wires. DQ/DQS lines go through U5/U6, so
   `walk` sets DIR for them.
2. Run `hold off`. The idle levels should be:
   - CK low, CK# high, CKE low;
   - CS0# high, RAS#/CAS#/WE# high;
   - RESET# high. RESET# is released in `hold off`, which is harmless with no
     stick; `init` drives it low again.
3. The bench current should stay at a few mA throughout.

**If a reading is wrong:**
- **The wrong line goes high:** a swapped wire. Fix it now, while it costs
  nothing.
- **1.5 V on a line `walk` hasn't reached:** a short between neighbours, or a
  floating spare input.
- **~3.3 V on any header pin: STOP.** A translator is wired the wrong way
  round, or VCCB is on 3V3. It would destroy the DRAM.

---

## Step 8 — first power-up with the stick

1. Seat the stick in the fixture. Unplug and replug the Core2350B's USB:
   step 7's `hold off` left RESET# high, and `bus_init` drives it low again
   from boot.
2. Set the bench supply to 1.5 V with a **300 mA limit**, and switch on.
   - **Expect** tens to ~200 mA: 18 chips held in reset, plus the VREF
     divider.
   - **If it hits the limit:** switch off. With the termination gone, nothing
     should draw that much, so look for a short in the fixture, VDD on a
     signal pin, or the wrong stick.
3. At the headers, measure:
   - VDD = 1.45–1.55 V;
   - VREF = 0.75 V ± 15 mV;
   - VTT floating at something. Note the value; it's harmless with no
     resistors left.
4. Run `spd` and `temp` again. They should still answer (VDDSPD is
   separate).

---

## Step 9 — init (M7): watch it on the analyser

Start the logic-analyser capture, triggering on CKE rising. Then run `init`.

Expect:

```
initialised (DLL off, CL6, CWL6, BL8); MR0 .... MR1 .... MR2 .... MR3 ....; now in self-refresh
extended temperature range: yes (SRT on)
```

In PulseView:
1. RESET# low ≥ 200 µs, then 500 µs before CKE rises.
2. CK running for ≥ 5 cycles before CKE goes high.
3. Four MRS commands (CS#, RAS#, CAS# and WE# all low), ≥ 4 CK apart, in the
   order MR2, MR3, MR1, MR0.
4. ZQCL, then the gap, then SRE: CKE falls, and the clock is allowed to stop.

Bus current after `init` (in self-refresh): **IDD6**, a few mA per chip.
Expect it to sit below the current measured in reset.

The DRAM gives no acknowledgement of init: the only test is step 10. The
analyser trace is how you tell "the DRAM didn't take it" apart from "we didn't
send what we think".

---

## Step 10 — first burst (M8): *it lives*

```
> w 0 0123456789abcdef
> r 0
```

`r` prints the whole sampling window: about 14 half-beats, with
`<- expected data` against the 8 beats where the data should land, then the
burst.

**Success** looks like `0x00000000: 01 23 45 67 89 ab cd ef`, with DQS
toggling against the data beats.

**If it doesn't look like that:**

| Symptom | Likely cause | Try |
|---|---|---|
| The right bytes, shifted one or two beats early or late | RL guess is wrong (DLL-off RL = CL − 1 assumed) | Note the offset; adjust `DDR3_RL_DLL_OFF` / the window in `ddr3.h` |
| DQS never toggles, DQ all `ff` or all `00` | The DRAM isn't driving, so READ wasn't accepted, or the init didn't take | Check the read in the analyser (CS# low, RAS# high, CAS# low, WE# high); check DIR_DQS goes low (B→A) for the window |
| DQS toggles, data is garbage but repeatable | The write didn't land: DM0 not strapped, or DQS polarity/centring on write | Check DM0 = VSS; write `00..00`, then `ff..ff`, then `55..aa`, and look at which bits stick |
| DQS toggles, data changes from read to read | Sampling on an edge: tDQSCK is 1–10 ns, a lot at `tck 250` | Run at `tck 3000`; fit the B-side pull-downs |
| One bit always wrong | A DQ line has a wiring or pogo fault | Step 7's `walk` for that DQ; step 4's diode reading |
| Nothing works, at every tCK | MR0 DLL-reset might matter after all | Add MR0 A8 = 1 and a 512-CK wait, as `datasheet-notes.md` §4 suggests |

Then run `t 0 1000`. Expect `1000 of 1000 bursts read back correctly`.

*Milestone 8. A desktop DDR3 DIMM, bit-banged from a microcontroller.
Take a photo.*

---

## Step 11 — soak (M9)

- Run `t 0 100000`, then leave the board idle in self-refresh for an hour.
  Then run `v 0 100000`, which reads the range back without writing it again.
  Expect `100000 of 100000 bursts read back correctly`.
- Note `temp` before and after. From 80 °C the firmware switches to 2× refresh
  by itself and caps tCK at 1.7 µs, back below 78 °C. If the SPD doesn't
  claim the extended range, it refuses to run hot instead.
- For a warm test, a hairdryer at a distance works: watch `temp` climb past
  80 °C, then repeat `t`/`v`. It should still pass, slower.

---

## Step 12 — the ESP32 (M10): *an ESP32 with 128 MB of RAM*

1. Wire the ESP32-S3 to the Core2350B as listed in `firmware/README.md`
   ("ESP32 link"), with a common ground. Both are 3.3 V.
2. Flash `esp32-client`:

   ```
   cd esp32-client
   idf.py set-target esp32s3 build flash monitor
   ```
3. Expect `RP2350 link v1: 128 MB, tCK 1000 ns`, then the init, then progress
   lines every 4 MB at up to about 1 MB/s. A full pass takes a few minutes
   each way.
4. At the end, expect `PASSED: 0 bad words in 128 MB`. In the RP2350 shell,
   `link` shows the same frame and error counts.

**If something goes wrong:**
- **`timed out waiting for READY`:** READY isn't wired, or the RP2350 isn't
  running the firmware. With nothing attached, the ESP32's pull-down keeps
  READY low.
- **`corrupt reply`, or a climbing retry count:** shorten the wires, or drop
  `clock_hz` in `main.c` to 2 MHz.
- **Bad words but no link errors:** the link is fine and the DRAM isn't.
  Go back to `t` in the shell.
