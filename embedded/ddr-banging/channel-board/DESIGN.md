# Channel board — design (SINT-100 / SINT-101)

A single PCB that replaces the pogo fixture + breakout boards + breadboarded
modules: a **240-pin DDR3 DIMM socket wired to a bare RP2354B**, with its own
power, USB-C for flashing, and an off-board link to the ESP32. All eight byte
lanes are routed through lane-switching translators; **only lane 0 is fitted**
on the first order. One board covers phase 1 (one lane) and phase 2 (lane
switching) of `../PLAN.md`; it is the first version of the phase 3 "channel
board".

Agreed in conversation, 2026-10-10.

## Goals

- Plug in a DDR3 UDIMM and a USB-C cable; flash the RP2354B; read the SPD; then
  work through SINT-100's milestones (init with DLL off, first burst, refresh,
  ESP32 link) with **no hand wiring**.
- **Fully assembled by JLCPCB**, DIMM socket included. Nothing for Martin to
  solder.
- Lanes 1–7 can be fitted later (by re-ordering with them in the BOM) without a
  board re-spin.

## Non-goals

- DDR3 firmware (SINT-100 milestones).
- Fitting lanes 1–7 on the first order.
- Phase 3 (several RP2354Bs in lockstep) and near-memory compute.
- The ECC lane (CB0–7, ninth chip) — left unconnected.
- On-board ESP32.

## Variant: single lane (option 1)

Priced alongside for comparison. Identical except: no lane-select decoder or DM
dividers, lanes 1–7 not routed, DM0 driven by GPIO (or tied low), possibly 2
layers. The socket still sets the board size, so savings are expected to be
small. Decision after pricing; everything below describes option 2.

## 1. Block diagram and power

```
 USB-C ──┬─ 5 V ─┬─ 3.3 V LDO ── RP2354B, translator A sides, SPD, expander
         │       ├─ 1.5 V buck (3 A) ── DIMM VDD/VDDQ, translator B sides
         │       │        └─ TPS51200 ── VTT 0.75 V (switchable) + VREF 0.75 V
         └─ D+/D− ── RP2354B USB (flashing, CDC console)

 RP2354B ──► C/A translators (fixed direction) ─────────────────┐
         ◄─► shared DQ/DQS bus ─┬─ lane 0 translator (fitted) ──┤
                                ├─ lanes 1–7 (DNP) ─────────────┤ DDR3 DIMM socket
         ──► 74LVC138 ──────────┴─ per-lane OE# + DM ───────────┤
         ◄─► I2C ── SPD EEPROM, PCA9534 expander (RESET#, VTT_EN, LED)
         ◄─► ESP32 header (SPI + READY)
         ◄─► SWD (JST-SH) + 2×25 all-GPIO header
```

### Power

| Rail | Source | Feeds |
|---|---|---|
| 5 V | USB-C, CC1/CC2 5.1 kΩ to GND (sink, up to 3 A from a capable charger) | regulators, ESP32 header |
| 3.3 V | LDO, ≥ 500 mA | RP2354B IOVDD, translator VCCA, decoder, expander, SPD (VDDSPD) |
| 1.1 V | RP2354B internal SMPS + 3.3 µH inductor (Pico 2 reference) | RP2354B core |
| 1.5 V | Synchronous buck, 3 A, 5 V in | DIMM VDD/VDDQ, translator VCCB, VTT regulator input |
| VTT 0.75 V | TPS51200, EN from expander | DIMM VTT pins (stock termination) |
| VREF 0.75 V | TPS51200 REFOUT | VREFCA, VREFDQ. Fallback: 1k/1k divider + 100 nF, DNP |

- **Termination:** with VTT on, stock (unmodified) sticks are tried first. If
  the translators cannot drive ~40 Ω to VTT cleanly, switch VTT off and use a
  stick with its termination arrays removed (the original plan).
- **Reset:** DIMM RESET# pulled low by resistor; released by the expander after
  rails are up (JEDEC power-up sequence).
- **ODT0/ODT1** tied low (on-die termination never used). **CKE0/CKE1** joined
  to one GPIO (both ranks enter/exit self-refresh together).
- Bulk + per-pin decoupling on 1.5 V at the socket; 100 nF per translator per rail.
- Power LED on 3.3 V; status LED on the expander.

### RP2354B support (Pico 2 reference design)

12 MHz crystal + load caps, 3.3 µH SMPS inductor, decoupling, USB 27 Ω series
resistors, BOOTSEL and RUN buttons, SWD on a Raspberry Pi 3-pin JST-SH plus
0.1" pads. 2 MB flash is in-package.

**Flashing:** blank flash boots the ROM USB bootloader (drag-and-drop `.uf2` or
`picotool load -x`); afterwards BOOTSEL + RUN, or `picotool` reboot via the
SDK's USB reset interface. SWD via the debug probe for debugging.

## 2. Signals, translators, lane switching

**One translator part throughout: SN74AVC16T245** (16-bit, two 8-bit halves,
independent DIR/OE per half, VCCA 3.3 V / VCCB 1.5 V; DIR and OE referenced to
VCCA).

- **Command/address — 2 fitted, fixed direction A→B.** 25 signals: CK, CK#, CKE,
  CS0#, CS1#, RAS#, CAS#, WE#, BA0–2, A0–A13. OE tied low, DIR tied high. Spare
  channels' inputs tied to GND.
- **Data — 1 per lane (8 footprints, lane 0 fitted).** Half 1: DQ0–7 of that
  lane. Half 2: DQS, DQS# (spare inputs tied off). Both halves' OE# from the
  lane's decoder output.
- **Shared bus:** the A sides of all eight lane translators connect to the same
  ten RP2354B pins (DQ0–7, DQS, DQS#). Only the selected lane is enabled, so
  chips on other lanes drive into disabled translators on READ.
- **Decoder:** 74LVC138 at 3.3 V; three GPIOs select a lane; enables tied active.
  Each active-low output drives (a) that lane's translator OE#s and (b) that
  lane's DM through a 1 kΩ / 820 Ω divider (3.3 V → ~1.49 V). Selected lane:
  enabled and unmasked; the rest: isolated and masked (their DQ float on WRITE,
  DM high discards it).
- **DIR:** two GPIOs — DIR_DQ to every lane's DQ half, DIR_DQS to every lane's
  DQS half.
- Ranks 0 and 1 share each lane's DQ on the module; CS0#/CS1# picks the rank.

### Pin map

PIO blocks see a 32-pin window based at GPIO 0 or GPIO 16.

| GPIO | Signal | Notes |
|---|---|---|
| 0–7 | DQ0–DQ7 | data PIO (window 0–31) |
| 8, 9 | DQS, DQS# | data PIO |
| 10 | DIR_DQ | |
| 11 | DIR_DQS | |
| 12–14 | LANE0–2 | decoder A/B/C |
| 15 | spare | to header |
| 16 | CK | command PIO (window 16–47); also visible to data PIO |
| 17 | CK# | |
| 18 | CKE | both ranks |
| 19, 20 | CS0#, CS1# | |
| 21–23 | RAS#, CAS#, WE# | |
| 24–26 | BA0–BA2 | |
| 27–40 | A0–A13 | A10 = AP |
| 41 | READY | to ESP32 |
| 42, 43 | I2C1 SDA, SCL | SPD (0x50), PCA9534; pull-ups to 3.3 V |
| 44–47 | SPI RX, CSn, SCK, TX | ESP32 link; SPI1 function pins, or PIO |

The command and data state machines live in different PIO blocks and are
synchronised with RP2350 cross-block IRQs (to verify — see Open items).

### Headers

- **ESP32:** 2×4 0.1" — SCK, MOSI, MISO, CS, READY, 3.3 V (reference), 5 V
  (can power the ESP32 board), GND.
- **All-GPIO:** 2×25 0.1" — GPIO 0–47 interleaved with GND; the logic analyser
  connection and hand-wiring escape hatch (all 3.3 V side).
- **SWD:** JST-SH 3-pin + 0.1" pads.
- **USB-C** on a short edge.

## 3. Build pipeline

```
channel-board/
  DESIGN.md       this file
  board/          Python package: parts, netlist model, circuit, placement,
                  .kicad_pcb / .kicad_pro / Specctra DSN writers, SES reader, fab
  build.sh        circuit → placed board → Freerouting → DRC → Gerbers/BOM/CPL
  footprints/     easyeda2kicad conversions (socket, USB-C, RP2354B, …)
  out/            gerbers.zip, bom.csv, cpl.csv, renders, ERC/DRC reports
```

1. **Parts** from JLCPCB's library, preferring Basic and in-stock parts;
   footprints from KiCad's library where they match, else LCSC/EasyEDA via
   `easyeda2kicad` (so JLCPCB's orientation preview matches).
2. **Circuit** in our own small Python netlist model (parts, pins, nets) with
   electrical checks. SKiDL dropped: we write the board file directly, so it
   would only add a dependency.
3. **Placement** scripted, `gen_pcb.py`-style: socket along a long edge with
   latch clearance; each lane's translator beside its byte group on the DIMM;
   C/A translators by the address pads; RP2354B central; USB-C on a short edge;
   headers on the far edge. Components ≥ 2.5 mm from edges except edge
   connectors.
4. **Board:** about 175 × 90 mm — the socket's ejectors span 165 mm open.
   **Stack-up (4 layers):** L1 signal, L2 solid GND, L3 power (5 V / 3.3 V /
   1.5 V pours), L4 signal.
5. **Routing:** Freerouting; then KiCad DRC against JLCPCB 4-layer rules, with
   margin: 0.15 mm track/space, 0.3 mm drill vias.
6. **Outputs:** Gerber zip, BOM and CPL with lanes 1–7 DNP.

**Routing route (confirmed 2026-10-10):** KiCad 10 has no SWIG `pcbnew` module and
`kicad-cli` has no Specctra export/import, so we write the DSN from our own
board model, run Freerouting 2.5.0 (`freerouting-2.5.0.jar`, Java 25) headless,
parse its `.ses`, and write the tracks and vias into the `.kicad_pcb`.
`kicad-cli pcb drc --refill-zones --save-board` fills the planes and checks.

**All components on the top side** (single-sided assembly).

## 4. Verification, cost, bring-up

### Before ordering (automated)

- **DIMM pinout check:** every one of the 240 socket pins' nets checked against
  the JEDEC DDR3 UDIMM pinout table (extending `../pogo-pins.md` to all pins).
- **GPIO map check:** every RP2354B pin against the table above; PIO groups
  contiguous and inside their windows.
- **Electrical checks** (netlist model), **DRC** zero errors and zero unconnected.
- **BOM check:** every fitted part has an in-stock LCSC number; cost totalled
  from the current price list.
- **Renders** of both sides.

### By Martin

- Look over the routed board (KiCad or renders).
- Check JLCPCB's placement preview at order time — RP2354B, translators, USB-C
  orientation especially.

### Cost

Priced separately (option 2 at 4 layers; option 1 at 4 and 2 layers) before
committing. Assembling only some of the five PCBs is an option.

### Bring-up checklist (when boards arrive)

1. No stick: 5 V, 3.3 V, 1.5 V, VREF, VTT (with VTT_EN toggled).
2. Flash a blink over USB.
3. Read the SPD over I2C.
4. Continuity walk of every C/A and DQ line (all-GPIO header + meter, or firmware).
5. Hand over to SINT-100 milestones: init (DLL off), first burst, refresh.

## Open items (resolve while building)

- SN74AVC16T245 drive strength at VCCB = 1.5 V vs ~40 Ω to VTT; LCSC stock.
- TPS51200 REFOUT stays on with EN low (keeps VREF when VTT is off).
- RP2354B BOOTSEL: is QSPI_SS bonded out with in-package flash? If not, use the
  documented alternative bootloader entry.
- RP2350 PIO cross-block IRQ (PREV/NEXT) for command/data SM sync.
- DDR3 DIMM socket: **TE 1-1932000-1 (JLCPCB C5171640)** — pre-order (0 stock,
  min 2, ~$6.84 each); wave-soldered, Economic or Standard PCBA. TE customer
  drawing C-1932000 rev H7 (`docs/ENG_CD_1932000_H7.pdf`, local): black housing,
  natural extractors, 15 µ" gold, 3.18 mm tails (TE's length for 2.36 mm PCB —
  ~1.6 mm protrusion on our 1.6 mm board, acceptable; **1-1932000-0, JLCPCB C5164821**, is the
  2.67 mm-tail twin for 1.6 mm PCB — **preferred**; same drawing).
  Footprint (sheet 3): 240 × Ø0.70 ± 0.08 mm on 1.00 mm pitch, staggered rows,
  max 0.15 mm annular ring; Ø2.45 mm peg holes at both ends (datums D, E) and
  at the key, plus a 1.80 mm slot; keep-out 142 × 7.75 mm; extractor shadow
  areas both ends (≤ 6.85 mm component height); 165 mm with latches open.
  Cross-check against JLCPCB's EasyEDA footprint (via easyeda2kicad).
  Fallback: Amphenol FCI 10081530-11107LF (C4775483) — DDR3 centre key, 2.67 mm
  tails, but consign-only at JLCPCB.
- I/O expander: TCA9534A out of stock at LCSC; PCA9534PWR (C133803) or PCA9554.
- SN74AVC16T245DGGR = C148481 (~490 in stock — reserve via My Part Lib).
- Is CK0/CK0# on the stick terminated differentially or to VTT?
- KiCad 10 scripting route for Freerouting (above).
