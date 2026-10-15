# DDR-banging (SINT-100)

Driving a desktop DDR3 DIMM directly from an RP2350B, so that an ESP32 can have
gigabytes of RAM. Plane epic: **SINT-100**.

| Path | What |
|---|---|
| `RUNBOOK.md` | Bring-up, step by step: parcel to first burst, with what "good" looks like at each step |
| `EXECUTOR.md` | Design note for the PIO executor (milestone 11): SM layout, word streams, timing, draft programs |
| `firmware/` | RP2350B bring-up firmware: shell, DLL-off DDR3 sequences, ESP32 link (slave) |
| `proto/` | `ddrlink`: the ESP32 ↔ RP2350B SPI protocol, shared by both sides |
| `esp32-client/` | ESP-IDF component `ddrbang` (the ESP32's API to the DIMM) and the 128 MB write/verify test |
| `pogo-pins.md` | Which DIMM pads the fixture contacts, and what each carries |
| `fixture/dimm-pogo-fixture.scad` | Resin pogo-pin fixture (Elegoo Mars 5 Ultra): two jaws, floor, end stops, notch key, hole test strip, insertion gauge |
| `fixture/pcb/gen_pcb.py` | Generates the two breakout PCBs (KiCad) from the `.scad`'s numbers |
| `fixture/pcb/art/` | Silkscreen cats: Tau (front board), Kappa (back board) |

## Rebuilding

```sh
# STLs
openscad -D 'part="front"' -o fixture/stl/front.stl fixture/dimm-pogo-fixture.scad   # also back | test | gauge

# PCBs, DRC, Gerbers for JLCPCB
cd fixture/pcb
python3 gen_pcb.py
kicad-cli pcb drc front.kicad_pcb        # and back
kicad-cli pcb export gerbers --layers F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,Edge.Cuts --no-x2 --subtract-soldermask -o gerbers-front/ front.kicad_pcb
kicad-cli pcb export drill --format excellon --excellon-separate-th -o gerbers-front/ front.kicad_pcb
```

Change geometry in the `.scad` only; the PCB generator parses pin lists, pad
positions and bolt positions from it, so the jaw and the boards cannot disagree.

## Key numbers

- Test stick: Samsung M391B5673FH0-CH9 (2 GB 2Rx8 ECC UDIMM, K4B1G0846F chips).
- Gold fingers 2.4 mm tall (measured); pin rows at 0.75 and 2.03 mm above the edge.
- P75-B1 pogo pins; 1.15 mm plated holes (JLCPCB PTH tolerance +0.13/−0.08).
- Four M3×25 bolts clamp PCB–jaw–floor–jaw–PCB.
- PCBs ordered from JLCPCB 2026-10-10: 1.6 mm, front black, back white.
