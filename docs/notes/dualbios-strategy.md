# DualBIOS — strategy for not bricking the GA-M68MT-S2 (SINT-34 → SINT-39)

Working notes for the BIOS dump (SINT-34), the eventual custom BIOS work
(SINT-39), and how to survive Gigabyte's automatic recovery in between.

Distilled from cross-referenced LLM assessments (ChatGPT, Grok) plus the
in-circuit dump attempts already logged in this branch. As with
`pci-bar-execute.md`: nothing here is verified on silicon yet; revise as
the real hardware surprises us.

## 1. What DualBIOS actually does (best current understanding)

Gigabyte doesn't document DualBIOS internals. Cross-referencing what's
reverse-engineered in the community and what the LLM assessments converged
on:

- DualBIOS is **not a pure-hardware checksum gate**. The CPU still begins
  executing the reset vector from M_BIOS regardless of any byte-sum.
- The mechanism is a **selector + watchdog**:
  - A small selector (probably implemented in the chipset / MCP68 or a
    helper IC) decides which of the two flash chips appears at the boot
    address space on this boot.
  - A watchdog expects the running BIOS to reach a "POST OK" heartbeat
    within some window. If it doesn't (M_BIOS hung, didn't initialise
    chipset enough to tick), the selector flips on next boot.
  - Once on B_BIOS, the board may copy B → M to "auto-repair" what it
    decided was broken.
- The **Award two's-complement-sum-to-zero checksum** is an internal Award
  BIOS sanity check, not a hardware gate. By the time it's evaluated, the
  CPU is already running code from M_BIOS. Patching that byte to make the
  sum zero is sometimes necessary for the Award image's own internal
  checks; it does **not** unlock or bypass any hardware DualBIOS validation.

The headline risk for SINT-39: if a broken custom BIOS is written to M_BIOS,
the board may "helpfully" overwrite it from B_BIOS mid-experiment. If both
chips have the broken image, it loops between two broken BIOSes and looks
bricked.

## 2. Workflow rules to follow without exception

1. **Dump both M_BIOS and B_BIOS first**, out of circuit, triple-read +
   sha256-verified. Two known-good dumps are the *only* recovery image; if
   they differ, investigate before doing anything else. (This is what
   `scripts/dump-verify.sh` in the bios-rw tree already does — see
   `embedded/bios-rw/README.md`.)
2. **Keep B_BIOS pristine.** Never write anything experimental to B_BIOS
   until a candidate image has cleanly POSTed for multiple reboots in
   M_BIOS.
3. **Treat the board as recoverable only via the external SPI programmer.**
   Don't rely on Gigabyte's Q-Flash, software flashers, or the in-board
   auto-recovery — those are precisely the systems we may have just
   confused.
4. **Physically isolate or remove B_BIOS while experimenting** (see §3).
   This is the strongest single intervention against the recovery-loop
   bricking mode.
5. **Always work on a copy of the dump**, never the verified original.
   Keep `m_bios.verified.bin` and `b_bios.verified.bin` immutable.

## 3. Isolating B_BIOS during experiments

Going beyond "keep it stock" and actually taking B_BIOS out of the loop:

- **Option A — desolder B_BIOS entirely.** Cleanest. The board now has no
  backup to recover from, so M_BIOS is the only thing in play. Pop B_BIOS
  into the SOIC8→DIP8 socket on the ESP32 rig if you want to read or
  re-verify it. Reinstall once SINT-39 is solid.
- **Option B — lift only B_BIOS's CS# pin.** The DualBIOS selector can no
  longer talk to B_BIOS so any copy-B-to-M sequence fails harmlessly. Less
  disruptive than full desolder, but small pin = pad-lift risk.
- **Option C — socket both chips.** Solder a SOIC-8 socket in place of each
  flash, then any chip swap becomes tool-free. Higher up-front effort, but
  if SINT-39 is going to involve repeated reflash cycles this pays back
  quickly. Worth costing in.

Option A is the recommended default for SINT-39. Option C is the
long-term answer if Sinter goes anywhere serious.

## 4. Before desoldering: voltage check at the chip

When the in-circuit clip read returns "No EEPROM/flash device found", one
failure mode is back-feeding through the chipset's protection structures:
the ESP32's 3V3 looks fine at idle but collapses to ~2.1 V during SPI
activity because the unpowered chipset becomes a parasitic load. The flash
never gets stable VCC and can't respond.

**Five-minute diagnostic** before going for the desolder:

1. Attach the clip with all pins connected as usual.
2. Hook a multimeter (or scope) **directly on the chip's VCC pin (pin 8)**.
3. Run `flashrom -p serprog:... --flash-name` (a probe that triggers SPI
   activity).
4. Watch the voltage. If it drops more than ~200 mV from 3.3 V during the
   probe, the chipset is dragging the rail down and no amount of
   spispeed-lowering will fix it.

This doesn't change the agreed plan (desolder), but it adds an objective
data point: if VCC is rock-solid 3.3 V and the chip *still* refuses to
respond, the contention is on the SPI lines (MISO most likely) rather
than on power, and the **MISO pin-2 lift** in `pci-bar-execute.md`'s
sibling context — *no, wrong file* — covered in the BIOS readme is the
right intermediate step. If VCC is collapsing, lifting MISO won't help and
desolder is the only path.

## 5. Recovery from a bad flash

If, despite the above, a custom M_BIOS bricks the board into a loop:

1. **Power off, unplug, drop the coin cell**, leave for a minute (clears
   any latched state in the selector / RTC well).
2. **Physically remove or isolate B_BIOS** if it isn't already (see §3) so
   the auto-recovery can't keep clobbering M_BIOS.
3. **Reflash M_BIOS out of circuit** with `m_bios.verified.bin` (the
   original Gigabyte image) via the ESP32 serprog rig + SOIC8→DIP8 socket.
4. Reinstall M_BIOS. Confirm POST.
5. Reinstall B_BIOS (still pristine) only after the board has been stable
   on the stock M_BIOS for several reboots.

The whole strategy depends on step 0 — the trusted dump — being **already
in hand** before anything experimental happens. This is why SINT-34
(getting a verified dump) gates everything else.

## 6. Things explicitly **not** to do

These came up in LLM assessments or other forum lore and are either wrong,
risky, or specific to a different board:

- **ATX standby "live-ground" trick** (plugging the 24-pin in to wake the
  3V3SB rail). On nForce platforms this wakes the chipset's own SPI master
  — you swap a passively-clamping unpowered chipset for an actively-driving
  alive one that owns the BIOS bus. Skip.
- **Award two's-complement checksum patcher as a "DualBIOS bypass".** The
  Award checksum is internal to the BIOS code, not a DualBIOS hardware
  gate. The patcher may be needed for the Award image's own sanity check
  to pass; it does not unlock the recovery system.
- **`invd` as a self-modifying-code safeguard.** `invd` discards modified
  cache lines without write-back and is a footgun for any kernel that's
  also using cacheable memory. Use `wbinvd` if a full flush is genuinely
  needed; usually a properly UC-mapped BAR makes the question moot.
- **"Some Gigabyte boards have a DualBIOS DIP switch / shorting jumper to
  force backup."** Not on the GA-M68MT-S2. Don't go looking. (Folklore
  imported from other manufacturers — ASUS CrashFree, certain MSI boards.)
- **Series-resistor / inject-stronger-drive trick on MISO.** Vague and
  risks damaging the chipset's input structures. Lift the leg or desolder
  instead.

## 7. What this file is *not*

A spec, a guarantee, or a substitute for a trusted dump. Every claim here
is "best current understanding"; revise as the real hardware (and SINT-34's
actual dump) tells us what's true.
