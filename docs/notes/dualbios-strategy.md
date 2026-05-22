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

1. **Dump both M_BIOS and B_BIOS first**, triple-read + sha256-verified.
   Two known-good dumps are the *only* recovery image; if they differ,
   investigate before doing anything else. The two paths to a dump are
   covered in §2a (try internal flashrom first) and §2b (clip / desolder
   as the fallback for B_BIOS or if internal fails).
2. **Keep B_BIOS pristine.** Never write anything experimental to B_BIOS
   until a candidate image has cleanly POSTed for multiple reboots in
   M_BIOS.
3. **For *writes*, treat the board as recoverable only via the external SPI programmer.**
   Don't rely on Gigabyte's Q-Flash, software flashers, or the in-board
   auto-recovery — those are precisely the systems we may have just
   confused.
4. **Physically isolate or remove B_BIOS while experimenting** (see §3).
   This is the strongest single intervention against the recovery-loop
   bricking mode.
5. **Always work on a copy of the dump**, never the verified original.
   Keep `m_bios.verified.bin` and `b_bios.verified.bin` immutable.

## 2a. Dumping via `flashrom -p internal` (no soldering required)

For a *read* of the currently-active chip (M_BIOS on a normal boot),
`flashrom`'s `internal` programmer talks to the BIOS flash through the
**chipset's own SPI master** — the same controller the MCP68 uses to boot
the board. Run it from a Linux live USB on the board itself; no clip, no
breadboard, no soldering iron. This should be the **first attempt** for
SINT-34.

```sh
# Boot any modern Linux live USB on the GA-M68MT-S2.
sudo apt install flashrom        # or dnf / pacman / xbps depending on distro
sudo flashrom -p internal -V                          # probe & identify
sudo flashrom -p internal -r dump1.bin
sudo flashrom -p internal -r dump2.bin
sudo flashrom -p internal -r dump3.bin
sha256sum dump1.bin dump2.bin dump3.bin               # same triple-read discipline
                                                      # as scripts/dump-verify.sh
```

Why this should work where the in-circuit clip failed:

- **No bus contention.** The chipset is alive and *expected* to be the SPI
  master; we're not asking the powered-down chipset to release the bus to
  a clip. The clamping war that defeated the in-circuit clip attempts
  simply doesn't happen.
- **Same flashrom chip driver** as the clip path
  (`MX25L1605A/MX25L1606E/MX25L1608E`) — once flashrom can reach the SPI
  bus, it identifies and reads the Macronix chip identically.
- **Read is non-destructive.** Worst case is "doesn't work, we still
  haven't damaged anything"; the clip rig is the fallback either way.

**The DualBIOS catch.** The MCP68 typically only exposes the
**currently-active** chip on the SPI bus the running CPU can see — so
internal flashrom reads M_BIOS, not B_BIOS, on a normal boot. To get
B_BIOS without external hardware you would have to force DualBIOS to
switch the active chip (commonly by deliberately corrupting M_BIOS and
rebooting), which is exactly the failure mode SINT-34 is trying to *avoid*
producing. So:

- **For M_BIOS:** internal flashrom is the recommended first try.
- **For B_BIOS:** the clip rig is still the right tool. Scope shrinks to
  one chip rather than two.
- **If `cmp m_bios b_bios` shows them identical** after internal flashrom
  reads M_BIOS and the clip reads B_BIOS, a verified M_BIOS dump *is* a
  verified B_BIOS dump. This is the common case on a board that has never
  experienced a DualBIOS recovery event.

**What could still go wrong with internal flashrom on this chipset**
(treat each as "verify, don't assume"):

- **Chipset write-protect lockdown.** Many boards set chipset-level
  write-protect bits during POST that make the flash read-only from the
  running OS. For a *read*, this doesn't matter. For a *write* (SINT-39),
  this is one of several reasons internal flashrom isn't the recommended
  write path. See §5a.
- **nForce / MCP68 chipset support in current flashrom.** flashrom 1.7.0
  on a recent Linux distro should support the MCP6x family; if it doesn't,
  the probe in step 1 above will say so explicitly. If it fails to find
  the chipset, fall back to the clip rig.
- **Inconsistent reads.** Run the triple-read and sha256 check exactly
  like `scripts/dump-verify.sh` does for the clip path. If the three
  reads disagree, *don't trust the dump* — investigate before proceeding.

## 2b. Dumping via the external SPI clip / desolder (the original SINT-34 plan)

Still the right tool for:

- **B_BIOS**, which internal flashrom can't see on a normal boot.
- **Any case where internal flashrom doesn't work** (chipset not
  recognised, inconsistent reads, write-protect getting in the way of
  diagnostic flags).
- **All of SINT-39's writes**, where the recovery story is "external
  programmer with the verified dump" — internal flashrom is *not* the
  write path even if it could write, because of the lockdown and
  DualBIOS-interference issues in §5a.

For the procedure see `embedded/bios-rw/README.md` and the in-circuit
contention findings already in this document (§4, §5).

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
than on power, and a pin-lift is the right intermediate step. If VCC is
collapsing, lifting a pin won't help and desolder is the only path.

### Which pin to lift: MISO vs /CS

Two distinct ways to break the chipset's control over the in-circuit
flash without removing it:

- **Lift pin 2 (MISO / DO)** — disconnects the chipset's *input* from the
  bus. The chip still hears the chipset's clock and CS toggling, but its
  output goes only to whatever the clip is wired to. Most direct fix for
  "JEDEC ID reads return garbage" since MISO is the contended line in the
  classic failure mode.
- **Lift pin 1 (/CS)** — disconnects the chipset's *addressing* of the
  chip. The chipset can no longer select it; only the clip can. Wider
  isolation than MISO-only (the chip is now invisible to the chipset for
  every signal), at the cost of needing to drive /CS from the clip too.
  A bit safer in that the chip can't be accidentally addressed by stray
  chipset activity.
- **Cut the /CS trace and bodge-wire it back later** — permanent
  equivalent of the /CS lift, useful if a clean pin-lift fails. Records
  a literal scar on the board.

The clean default is **MISO lift** when the failure mode is "no JEDEC ID";
**/CS lift** is the slightly stronger isolation if MISO lift still
returns garbage (suggests the chipset is also asserting CS at wrong
moments). Pads on this vintage of Gigabyte board lift easily — go in with
flux, magnification, and a fine tip.

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

## 5a. Why internal flashrom is *not* the recommended write path

`flashrom -p internal -w` could in principle write the BIOS through the
chipset's SPI master too, but for SINT-39 the external programmer is the
right tool. Three reasons:

1. **Chipset-level write-protect lockdown.** During POST, the running BIOS
   typically sets chipset bits that make the SPI flash read-only from the
   running OS. Defeating this requires either kernel options like
   `iomem=relaxed`, flashrom flags like `--force`, and/or modifying the
   early-BIOS path to leave the lock open — which is precisely the
   chicken-and-egg problem SINT-39 is trying to break in the first place.
2. **DualBIOS interference mid-experiment.** If a write through internal
   flashrom produces an M_BIOS that doesn't reach POST-OK on the next
   boot, the DualBIOS selector flips, B_BIOS becomes active, and the
   board may copy B → M, silently overwriting your work. The external
   programmer + physically-isolated B_BIOS (see §3) sidesteps both of
   those failure modes.
3. **Recovery still goes through the external rig anyway.** If the
   internal write goes wrong and the board won't POST at all, the *only*
   way back is the external SPI clip + the verified dump. So you need
   the external rig as the safety net even if you never use it for the
   happy path — which means the marginal value of also using internal
   flashrom for the write is essentially zero.

Internal flashrom for *writes* might become the right optimisation
*after* you have a custom BIOS image that already POSTs reliably and
you're just iterating quickly. Until then, the external programmer is
the deliberate, controllable, recoverable path.

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
