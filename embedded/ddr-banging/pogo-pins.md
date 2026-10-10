# Pogo pins — which DIMM pads, and what they carry

DDR3 240-pin UDIMM pinout (JEDEC; Micron MT16JTF table 5). Back pad 120+n sits
directly behind front pad n. The fixture (`fixture/dimm-pogo-fixture.scad`, `holes="needed"`)
drills exactly these holes: 38 per jaw, 76 pins.

Our stick: Samsung M391B5673FH0 (2Rx8 ECC) — rank 0, byte lane 0 (chip U1 + its back twin is rank 1).

## Front jaw (pins 1–120)

| Pin | Signal | Use |
|---|---|---|
| 1 | VREFDQ | 0.75 V reference (divider) |
| 2, 5, 8, 11, 80, 83 | VSS | Ground |
| 3, 4, 9, 10 | DQ0, DQ1, DQ2, DQ3 | Data (translator, bidirectional) |
| 6, 7 | DQS0#, DQS0 | Data strobe pair (translator, bidirectional) |
| 50 | CKE0 | Clock enable, rank 0 |
| 51, 54, 57, 60, 62, 65, 66 | VDD | 1.5 V |
| 52 | BA2 | Bank address |
| 55, 56, 58, 59, 61 | A11, A7, A5, A4, A2 | Address |
| 67 | VREFCA | 0.75 V reference (divider) |
| 70 | A10/AP | Address / auto-precharge |
| 71 | BA0 | Bank address |
| 73 | WE# | Command |
| 74 | CAS# | Command |
| 76 | S1# | Chip select rank 1 — **tie high** (resistor to 1.5 V) |
| 77 | ODT1 | ODT rank 1 — **tie low** |
| 117, 119 | SA0, SA2 | SPD address — tie to GND (→ 0x50) |
| 118 | SCL | SPD I2C clock (3.3 V side, direct to RP2350) |
| 120 | VTT | Termination supply — unused once termination is removed |

## Back jaw (pins 121–240)

| Pin | Signal | Use |
|---|---|---|
| 121, 124, 127, 130, 199, 202 | VSS | Ground |
| 122, 123, 128, 129 | DQ4, DQ5, DQ6, DQ7 | Data |
| 125 | DM0 | Data mask (tie low = always write, or drive) |
| 168 | RESET# | Reset |
| 169 | CKE1 | Clock enable rank 1 — **tie low** |
| 170, 173, 176, 179, 182, 183, 186 | VDD | 1.5 V |
| 174, 175, 177, 178, 180, 181, 188, 196 | A12, A9, A8, A6, A3, A1, A0, A13 | Address |
| 184, 185 | CK0, CK0# | Clock pair |
| 190 | BA1 | Bank address |
| 192 | RAS# | Command |
| 193 | S0# | Chip select rank 0 |
| 195 | ODT0 | ODT rank 0 — hold low (ODT off) |
| 236 | VDDSPD | 3.3 V for the SPD EEPROM |
| 237 | SA1 | SPD address — tie to GND |
| 238 | SDA | SPD I2C data |
| 240 | VTT | Termination supply |

Not wired: DQ8–63, DQS1–7, DM1–7, CB0–7 (ECC), CK1/CK1#, A14 (NC on 2 GB), the
other VDD/VSS pads.
