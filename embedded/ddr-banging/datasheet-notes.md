# Datasheet notes: slow-clock, DLL-off DDR3 on Samsung M391B5673FH0-CH9

Researched 2026-10-12. Target: 2 GB 2Rx8 PC3-10600E ECC UDIMM, Samsung K4B1G0846F (1 Gb x8, F-die) components, driven from an RP2350B (PIO) at a very slow CK with MR1 A0=1.

**Provenance caveat.** I could not retrieve the Samsung K4B1G0846F or M391B5673FH0 datasheets themselves (see section 1). Nearly all hard numbers below come from the **Micron 1 Gb DDR3 datasheet (MT41J128M8, Rev. L 03/13)**, which is the same density, organisation and JEDEC DDR3-800 to 1600 family, and from a Micron UDIMM datasheet. Samsung's F-die should match on JEDEC-mandated items, but any Samsung-specific limits (notably a maximum tCK in DLL-off) are unverified. Downloaded copies are in `/private/tmp/claude-501/ddr-research/` (`micron1g.pdf`, `mt9.pdf`), not in the repo.

## 1. Datasheet availability

**Finding.** No directly downloadable Samsung K4B1G0846F or M391B5673FH0 datasheet was found.
- alldatasheet has a K4B1G0846F page, but it returned HTTP 403 to automated fetching. Worth opening in a browser: https://html.alldatasheet.com/html-pdf/347881/SAMSUNG/K4B1G0846F/297/1/K4B1G0846F.html
- datasheetarchive has an entry: https://www.datasheetarchive.com/datasheet/69768009cb44bfec (not fetched).
- Samsung's old hosting path for the module sheet (`download.semiconductor.samsung.com/resources/data-sheet/926498m392b5673fh0_cf8_ch9_ck0_idt_b0.pdf`) is now 404. That filename is the M392 (registered) variant of the same family, so it is not the exact UDIMM sheet anyway.
- Samsung's 2010 DDR3 product guide lists K4B1G0846F as a 1 Gb, 128M x 8, 78-ball part, speed bins F8/H9/K0 (1066/1333/1600), 1.5 V. The product statnano page lists Samsung consumer DRAM: https://product.statnano.com/product/5495
- Closest sibling: Samsung K4B1G0446G/0846G (G-die, 1.35 V DDR3L) Rev 1.03, mirrored at https://www.electronicsdatasheets.com/download/52723092e34e248938c74385.pdf?format=pdf (the mirror served an HTML wrapper to curl; open in a browser).
- Samsung confirms F-die in this module family: a Samsung module datasheet lists K4B1G0846F-HCF8/HCH9/HCK0 as the component for the 2 GB 2Rx8 modules (M471B5673FH0 SODIMM sheet, https://download.semiconductor.samsung.com/resources/data-sheet/172567m471b5773chs_cf7_cf8_ch9_ck0.pdf, which I fetched but is only two pages).
- Usable stand-in documents that I did read in full:
  - Micron 1 Gb DDR3 (Rev. L): https://faculty-web.msoe.edu/johnsontimoj/ELE455/files455/ddr3.pdf
  - Micron 1/2/4 GB x72 ECC UDIMM sheet: https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/1300/MT9JSF12872AZ_MT9JSF25672AZ.pdf

**Confidence.** High that the Samsung sheets are not trivially fetchable; high that the Micron sheet is a valid proxy for JEDEC-defined behaviour; medium for anything Samsung-specific.

**Source.** URLs above.

## 2. DLL-off mode: clock limits, CL/CWL, tDQSCK

**Finding.**
- **Maximum frequency (minimum tCK) is 8 ns, i.e. 125 MHz.** Micron lists tCK(DLL_DIS) min = 8 ns for every speed grade. Lattice and Microchip also cite 8 ns / 125 MHz as the DLL-off ceiling.
- **Lower clock limit: there is a stated maximum period in Micron's sheet, and it is the refresh interval.** Micron's table gives tCK(DLL_DIS) max = **7800 ns** for Tc <= 85 C and **3900 ns** for Tc 85 to 95 C, i.e. 7.8 us / 3.9 us (equal to tREFI). So the practical floor is about 128 kHz (about 256 kHz when hot), not "no minimum". I could not confirm whether JEDEC JESD79-3 itself prints a maximum for tCK(DLL_OFF) (my recollection is that it shows 8 ns min and no max, but I did not read the standard). A Micron DDR4 sheet explicitly says "no minimum frequency" other than meeting tREFI; the DDR3 Micron figure is consistent with that same reasoning. Treat "operation below some frequency allowed" as: nothing formal, the limit is that you must still issue REF at tREFI average, and one clock period must not exceed tREFI.
- **CL and CWL are fixed at 6 and 6** in DLL-off ("only one value of CAS latency (CL = 6) and one value of CAS WRITE latency (CWL = 6)").
- **Read data timing.** tDQSCK(DLL_DIS) = **1 ns min, 10 ns max** (Micron Table 74), and it "could be larger than tCK". It starts **CL + AL - 1** clocks after the READ command (so RL(DLL_DIS) = 5 with AL=0), rather than CL + AL. DQS/DQ are no longer edge-aligned to CK. DQ-to-DQS (tDQSQ, tQH) is unaffected. At a slow CK this is easy: the 1 to 10 ns uncertainty is tiny against a half-period of microseconds. Sample DQ using DQS edges or simply at a fixed delay (several CK later) after RL.
- Micron warns it does "not warrant" normal-mode timings in DLL-off; tWR must be the greater of 4 CK or 15 ns.
- Other exceptions Micron lists: ODT not allowed; output no longer edge-aligned to clock.

**Confidence.** High for 8 ns minimum, CL=6/CWL=6, tDQSCK 1 to 10 ns (Micron datasheet). Medium for the 7.8 us maximum as a Samsung/JEDEC property. Low on whether JEDEC prints a maximum.

**Source.** Micron 1 Gb DDR3 Rev. L, pp. 56-57 (AC table, note 26), p. 123, p. 126 (Table 74): https://faculty-web.msoe.edu/johnsontimoj/ELE455/files455/ddr3.pdf ; Lattice answer https://www.latticesemi.com/support/answerdatabase/2/3/8/2386 ; Microchip https://onlinedocs.microchip.com/oxy/GUID-82119957-1E11-4B69-84AC-EF0EA08F5595-en-US-5/GUID-04C98076-0F6E-4FB7-8B18-CBCE0E6783B8.html (search snippet; page returned 503 on fetch).

## 3. Refresh at slow clock; self-refresh

**Finding.**
- **Confirmed:** average tREFI = 7.8 us (64 ms / 8192) at Tc <= 85 C, 3.9 us at Tc 85 to 95 C. Up to 8 REFs may be postponed, so the absolute gap between REFs can be up to 9 x tREFI = 70.2 us, and at most 16 REFs may be issued in 2 x tREFI. tRFC for 1 Gb is roughly 110 ns (check the Samsung bin), trivial at a slow clock, so one clock cycle covers it.
- Practical consequence: with a 1 MHz CK, a refresh every ~7 clocks (or batch 8 REFs every ~62 us). At 128 kHz-class clocks refreshing is essentially every single clock cycle. Also note tRAS max = 9 x tREFI (70.2 us), so a row cannot stay open longer than that.
- **Self-refresh does not need the DLL.** The DLL is automatically disabled on SRE and automatically re-enabled and reset on SRX *if it was enabled beforehand*. If DLL was disabled (MR1 A0=1) before SRE, it **stays disabled** after SRX.
- **Exit:** tXS = max(5 CK, tRFC + 10 ns) for commands not needing a locked DLL (writes, ACT, REF, etc.). tXSDLL = tDLLK(min) = 512 CK for commands needing a locked DLL (READ, synchronous ODT). Micron also says a ZQCL must be issued and tZQOPER met before a command needing locked DLL. In DLL-off mode the DLL-locked-dependent requirement is arguably moot, but the datasheet does not say tXSDLL is waived; the safe approach is to wait 512 CK (cheap at slow clock only in count; at 1 MHz this is 512 us, harmless).
- SRE needs a stable valid clock; if the clock is held unchanged during self-refresh, tCKSRE/tCKSRX do not apply but tCKESR does. Entering self-refresh is a useful safety net: once in SR, only CKE and RESET# matter, and the DRAM refreshes itself, so the spike can park the DIMM in SR whilst the PIO program is reloaded.
- Self-refresh also is the only legal point to cross the clock-rate gap between DLL-on and DLL-off.

**Confidence.** High (Micron datasheet; same JEDEC text).

**Source.** Micron 1 Gb DDR3 Rev. L, pp. 56-57 (tXS, tXSDLL, tREFI), p. 123 to 124, p. 141 to 142, SELF REFRESH Operation p. 180, REFRESH command text ~p. 100: https://faculty-web.msoe.edu/johnsontimoj/ELE455/files455/ddr3.pdf

## 4. Power-up / initialisation with DLL off

**Finding.**
- JEDEC/Micron sequence: power up with RESET# low (below 0.2 x VDDQ during ramp); RESET# low >= 200 us after power is stable; CKE low >= 10 ns before RESET# rises; after RESET# rises wait 500 us (minus one clock) with CKE low; then CKE high with clock valid >= 10 ns (and >= 5 clocks) and ODT driven low at least tIS before CKE high; only NOP/DES until tXPR = max(5 CK, tRFC + 10 ns) has passed; then MRS to MR2, MR3, MR1 (with DLL enable and ODT configuration), MR0 (with DLL RESET), ZQCL; wait tZQinit = 512 CK and tDLLK = 512 CK; then ready. tMRD (4 CK) between MRS, tMOD = max(12 CK, 15 ns) before non-MRS commands.
- **Initialising straight into DLL-off.** The standard init is written assuming the DLL is *enabled* ("DLL enable is required during power-up initialization"). The Micron text says "MR1[0] can be switched during initialization or later", so setting MR1 A0=1 during the MR1 step is explicitly allowed. It does not describe a distinct "init into DLL-off" sequence. The canonical DLL-off entry is the on-to-off procedure below, but the spike's direct approach (MR1 A0=1 at init) is what ultraembedded's controller and the Lattice/Microchip controller docs do ("Enabling it at initialisation requires setting the mode register bit..." per Microchip).
- **MR0 DLL RESET with DLL off.** The datasheet does not say DLL RESET is forbidden or required in DLL-off. Reasoning: DLL RESET only matters to lock the DLL, which is not used. I would omit MR0[8] (set to 0) and not wait tDLLK for reads. Still honour tZQinit (512 CK minimum): ZQCL can run in parallel with DLL reset. Risk: low-medium; if reads misbehave, add DLL RESET + 512 CK as an experiment.
- **DLL-on to DLL-off switching (when starting from a DLL-on state):** 1) from idle with ODT off and RTT_NOM/RTT_WR = 0, set MR1[0]=1; 2) wait tMOD, enter self-refresh; 3) after tCKSRE, change the clock; 4) exit SR once the new clock is stable for tCKSRX; after tXS update the mode registers with DLL-off settings (CL=6, CWL=6, WR >= 15 ns); 5) wait max(tMRD, tMOD) and issue ZQCL. Back to DLL-on: set MR1[0]=0, MR0[8]=1 (DLL RESET), wait tDLLK (512 CK) before any READ. The clock may only cross the gap between the DLL-on maximum period (a few ns, per speed bin) and tCK(DLL_DIS) min (8 ns) during self-refresh (or precharge power-down).
- Important for slow clock: **RESET# low at least 200 us and CKE low 500 us are wall-clock times** and are independent of CK; with 7.8 us CK this means ~65 clocks, but CK must still be present and valid for 5 clocks before CKE rises. ZQinit (512 CK) and tDLLK (512 CK) are in clocks, so they stretch with a slow clock. At 1 MHz, tZQinit is 512 us, fine; at 128 kHz about 4 ms.
- Also: MRS with CL/CWL mismatch: DLL-off requires MR0 CL=6 (A[6:4]=010 and A2=0 encoding) and MR2 CWL=6 (A[5:3]=001); AL=0 recommended.

**Confidence.** High on the standard sequence and the on-to-off procedure. Medium on omitting MR0 DLL RESET in DLL-off (inferred, not stated).

**Source.** Micron Rev. L pp. 123 to 126 (DLL disable mode), p. 134 to 136 (initialisation), p. 139 (DLL RESET), p. 141 to 142 (DLL enable/disable), pp. 127 (input clock frequency change): https://faculty-web.msoe.edu/johnsontimoj/ELE455/files455/ddr3.pdf ; Microchip controller DLL-off notes https://onlinedocs.microchip.com/oxy/GUID-82119957-1E11-4B69-84AC-EF0EA08F5595-en-US-5/GUID-8C900B6D-D875-4FE9-A89F-2D520C6AE453.html (snippet only).

## 5. On-module termination, VTT

**Finding.**
- From Micron's x72 ECC UDIMM datasheet (functional block diagram and text): clock, control, command and address buses are routed **fly-by** and terminated at the end of the fly-by on the module ("terminated control, command, and address bus").
- **CKE0, A[15:0], RAS#, CAS#, WE#, ODT0, BA[2:0], S0# terminate to VTT** through resistors (VTT is on the DIMM edge, pins 120/240 family, supplied by the host). Confirmed by diagram.
- **CK/CK# are not terminated to VTT.** The diagram shows the CK/CK# pair terminated with a resistor and capacitor (an AC termination) referenced to VDD, i.e. a differential/RC network rather than VTT resistors. Resistor value is not printed in the datasheet. (Reading of the diagram, medium confidence.) The Samsung 2Rx8 ECC raw card is likely similar (JEDEC raw card B/ECC), but I did not find the Samsung schematic; the CK0/CK1 pair per rank is the usual 2R arrangement.
- DQ and DQS lines have series resistors on the module and no termination on the module (termination is ODT in the DRAM or controller).
- **VTT role.** It is the parallel termination supply for the command/address/control fly-by (= VDD/2 = 0.75 V). Micron's UDIMM sheet specifies IVTT of +/-600 mA and VTT = 0.5 x VDD +/-2%. The component datasheet says "VTT is not applied directly to the device".
- **VTT unpowered vs 0.75 V.** Unpowered (floating): the termination resistors just hang off an undriven net, so effectively unterminated lines. For slow, low-edge-rate signals that is fine for signal integrity (reflections damp out between slow edges), but the floating VTT net plus its decoupling capacitors will be pulled around by the driven lines through the resistors, to roughly the average of the driven levels, and the resulting behaviour is undefined. Driven to 0.75 V: each address/command line that your driver holds at a level other than 0.75 V sources or sinks (V - 0.75 V) / R_term, with R_term typically a few tens of ohms (I did not find the value; roughly 36 to 47 ohm is my recollection, unverified). At a 1.5 V logic high that is around 15 to 20 mA per line, and about 25 to 28 such lines gives several hundred mA in the worst case. Micron's own spec of 600 mA is consistent with that. A level-shifter or buffer must be able to drive that, or the high level will sag. If VTT is left unpowered the line load is light, but the DRAM still sees valid VIH/VIL as long as drivers swing rail-to-rail.
- VDD must be valid, and Micron's power-up note says VTT is "limited to 0.95 V when the power ramp is complete" and tVTD >= 0 (VTT must not precede VDD, to avoid latch-up). Do not apply VTT before VDD/VDDQ.

**Confidence.** High that address/control go to VTT; medium for the CK pair RC network to VDD (diagram read); low on termination resistor values and on exactly what an unpowered VTT does to a given board.

**Source.** Micron MT9JSF x72 ECC UDIMM datasheet: https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/1300/MT9JSF12872AZ_MT9JSF25672AZ.pdf (p. 9 block diagram; p. 10 text; electrical table for VTT/IVTT); Micron 1 Gb DDR3 p. 134 power-up conditions. NXP note (via search; page 403 on fetch): https://community.nxp.com/t5/i-MX-Processors/What-is-the-usage-of-DDR3-CLK-200ohm-resister-in-parallel/td-p/241385 ; Micron TN-41-08 text via https://dl.21ic.com/download/ddr3_design_guide-335428.html (search snippet only).

## 6. Prior art: DDR3 at very low clock

**Finding.**
- No report of anyone bit-banging DDR3 from a microcontroller, or running DDR3 at about 1 MHz, turned up in several searches. Absence of evidence only; the web search tooling is weak for this niche.
- Closest evidence that DLL-off works in practice: ultraembedded's open-source DDR3 controller (Xilinx 7-series and Lattice ECP5) says the DLL can be turned off ("in most DRAM parts") and run at <= 125 MHz, and that "testing proves that it works well". Tested on Arty A7 (MT41K128M16JT) and ECPIX-5 (MT41K256M16RE); performance figures were measured at 100 MHz. https://github.com/ultraembedded/core_ddr3_controller
- Microchip's SAMA5/SAM9X controllers support DLL-off, noting DLL-off operation up to 166 MHz on listed Micron parts; Lattice ECP3 FAQ says tCK max can be up to 8 ns. These describe 100+ MHz DLL-off, not kHz.
- Nothing found stating DDR3 fails below some frequency in DLL-off mode. The datasheet-level concern is only the 7.8 us (tREFI) period cap in Micron's table.

**Confidence.** Medium that DLL-off works at tens of MHz (community evidence). Low for the 1 MHz or kHz regime (no evidence either way).

**Source.** URLs above.

## 7. ODT

**Finding.** Confirmed. Micron: "The ODT feature (including dynamic ODT) is not supported during DLL disable mode"; the ODT resistors "must be disabled by continuously registering the ODT ball LOW by programming RTT,nom MR1[9, 6, 2] and RTT(WR) MR2[10, 9] to 0". So set MR1 A9/A6/A2 = 0 (RTT_NOM off), MR2 A10/A9 = 0 (RTT_WR off), and hold ODT low. During power-up, ODT should be driven low at least tIS before CKE is registered high. If both RTT settings are disabled, ODT is a "don't care" for self-refresh entry, but keep it low anyway. On the DIMM, ODT0 is also a VTT-terminated line.

**Confidence.** High.

**Source.** Micron 1 Gb DDR3 Rev. L p. 123: https://faculty-web.msoe.edu/johnsontimoj/ELE455/files455/ddr3.pdf

## Implications for the spike

Red flags and notes, most important first:

1. **Clock period cap.** Micron's sheet caps tCK(DLL_DIS) at 7.8 us (3.9 us above 85 C), so "kHz" clocks below roughly 128 kHz are formally outside the table. 1 MHz is inside it. Start at about 1 to 5 MHz and only go lower as a deliberate experiment. I could not verify the Samsung number.
2. **Samsung datasheet not read.** Get the alldatasheet copy of K4B1G0846F in a browser and confirm: tCK(DLL_OFF) min/max, tDQSCK(DLL_DIS), tREFI, tRFC, tXS/tXSDLL, MR0 DLL-reset wording. Do this before committing to design details.
3. **Refresh is the real-time constraint.** One REF per 7.8 us on average; the PIO program must interleave REF with any traffic. At a 1 MHz CK that is about every 7 clocks. Consider batching up to 8 postponed REFs, or park the DIMM in self-refresh (CKE low) between bursts of activity; SR does not need CK once entered.
4. **Open-row limit.** tRAS max = 9 x tREFI (about 70 us). A slow-clock burst scheme must PRE within that or use auto-precharge.
5. **VTT.** Decide explicitly: either supply 0.75 V (check the source can sink/source several hundred mA and the level shifters can drive the terminations) or leave floating and accept effectively unterminated command lines. Never put VTT up before VDD. CK/CK# appear to be RC-terminated to VDD, so a 1.5 V CMOS-driven differential CK is a heavy-ish load but fine at low MHz.
6. **Level shifting.** DDR3 is SSTL15 (VDD 1.5 V, Vref 0.75 V). RP2350 GPIO is 3.3 V or 1.8 V, so a translator and VREFCA/VREFDQ (0.75 V each) are needed. Read data from DQ/DQS must be shifted back down.
7. **Init in DLL-off.** MR1 A0=1 at init is legal ("can be switched during initialization"). I would skip MR0 DLL RESET and the tDLLK wait, but keep tZQinit = 512 CK. Parameterise it so it can be added if reads misbehave.
8. **CL=6, CWL=6, AL=0.** RL = 5 clocks effective in DLL-off. tDQSCK 1 to 10 ns is negligible vs a microsecond clock, so read capture on PIO is easy (sample at DQS edges or a fixed delay).
9. **ECC DIMM and rank select.** 2Rx8 with ECC: 18 devices, two ranks, so S0#/S1#, CKE0/CKE1, ODT0/ODT1 and CK0/CK1 pairs all exist. Drive both ranks' CKE and CS sensibly (tie CKE1 with CKE0 for the spike). All 9 devices in a rank share the command bus, so one rank is enough for the test; keep the other rank deselected. 72-bit data bus: DQ pins unused by an 8-bit test are simply ignored.
10. **Wall-clock init times** (RESET# low 200 us, CKE low 500 us) are independent of the slow CK; CK must be running with at least 5 valid clocks before CKE goes high.
11. **Write leveling** is optional in DLL-off at slow speed (the fly-by skew is nanoseconds against microsecond periods), so skip it. The DRAM will see writes with DQS aligned to CK within the tDQSS window, trivially satisfied.
12. **Prior art is absent** for 1 MHz or bit-banged operation. Expect to be the first to find out; keep a logic analyser on CKE, CS#, RAS#/CAS#/WE#, and DQS to separate "DRAM ignoring me" from "my timing is wrong".
