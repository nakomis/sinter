# Sinter project assessment — Microsoft Copilot

Pasted into the conversation as plain text; preserved verbatim below for
traceability alongside the ChatGPT and Grok transcripts.

---

You're doing something delightfully unhinged in exactly the right way, so I'll
skip the hand-holding and go straight to the sharp edges.

## 1. MCP68 vs north/southbridge, memory controller, HyperTransport

Your picture is basically right. MCP68 is a single-chip chipset that collapses
what used to be north + south into one package: PCIe root, legacy PCI,
SATA/IDE, USB, LPC, integrated graphics, etc.

The DDR2/DDR3 controller is on the Phenom II die. AM3 CPUs have the integrated
memory controller; the chipset never sees DRAM directly, only traffic over
HyperTransport.

FSB is gone; HT is the link. The CPU talks to MCP68 over a HyperTransport link
instead of a front-side bus. That's standard for K8/K10-era AMD.

Nothing in your mental model there is dangerously wrong.

## 2. Talking to MCP68 from a custom kernel

The realistic path is very "normal PC OS dev":

**PCI enumeration:**

- Use CF8/CFC config space access or MMCONFIG if you want to be fancy later.
- Enumerate bus 0, device 0 for the host bridge, then walk bridges. MCP68
  exposes the usual set of devices: host bridge, ISA/LPC bridge, IDE/SATA,
  SMBus, USB, PCI bridge, VGA, etc.

**Interrupts:**

- Early on, you can absolutely use the legacy 8259 PIC (via the MCP68's ISA
  bridge) and then switch to IO-APIC once you're comfortable parsing ACPI.
- Expect a standard IO-APIC + ACPI MADT; nothing exotic, just NVIDIA's usual
  slightly-quirky routing.

**Storage:**

- MCP68 can present SATA as legacy IDE (compatibility mode) or native
  SATA/AHCI/RAID. For a hobby kernel, forcing legacy IDE mode in BIOS and
  treating it as a PATA controller is the least painful path.
- NVIDIA's SATA/RAID modes are notoriously under-documented; avoid them until
  you're ready to reverse driver behaviour.

**Nasty surprises:**

- Documentation is the real enemy. NVIDIA never published a nice public MCP68
  datasheet; you're mostly inferring from Linux/FreeBSD drivers and PCI IDs.
- Some nForce chipsets have oddities around MSI/MSI-X and power management,
  but if you stick to legacy INTx and don't try to be clever with ASPM/ACPI
  C-states early on, you'll be fine.
- Expect to lean heavily on existing open-source drivers as "specs by
  example".

Nothing about MCP68 is uniquely hostile to hobby OS devs; it's just opaque
and old.

## 3. Executing from a PCI BAR backed by FPGA DDR3

This is the fun/fragile part.

### Cacheability and coherency

You must treat that BAR as non-cacheable from the CPU's point of view.

- If the Phenom II caches instructions from that region and the ESP32 writes
  new code behind its back, you're in undefined territory.
- Use MTRRs/PAT to map the BAR as UC (uncacheable) or at most UC-; do not
  map it WB.

x86 self-modifying code rules don't save you.

- The architectural guarantees assume the same core did the writes and then
  executes after a serializing instruction.
- An external writer (ESP32 via FPGA) doesn't participate in that protocol;
  the CPU has no idea its caches are stale unless you explicitly flush them.

Realistically: mark the region UC and accept the latency. If you ever
experiment with caching, you'll need explicit cache flushes (e.g., WBINVD or
CLFLUSH on the region) between "ESP32 finished writing" and "CPU starts
executing", and that's still brittle.

### Instruction prefetch and BAR boundaries

The CPU will prefetch past your "logical" boundaries.

- If the BAR is, say, 16 MiB, the CPU can still speculatively fetch within
  that range; it doesn't know about your internal layout.
- You should align code segments and, if possible, place a guard region
  (filled with safe traps like UD2) at the end of your executable area so
  that runaway prefetch doesn't wander into random data.

Don't rely on BAR size as a hard fence.

- The chipset will enforce address decoding, but speculative fetches inside
  the BAR are fair game.

### Synchronisation between ESP32 and Phenom II

You need a protocol, not vibes. For example:

**Double-buffered code regions:**

- Region A executing, Region B being written.
- ESP32 writes B, then sets a "ready" flag in a small control area.
- CPU polls that flag (also UC-mapped), sees it, jumps to B, then marks A free.

**Versioned descriptors:**

- A small control structure with a sequence number, code start, length, and
  maybe a checksum.
- ESP32 writes code, then updates the descriptor and increments the sequence
  number last.
- CPU polls until it sees a stable sequence number, then executes.

**Ordering:**

- Make sure the ESP32 writes code first, then the "ready" flag/descriptor
  last (with whatever memory barrier its architecture provides).
- On the CPU side, read the descriptor/flag, then execute; if you ever cache
  that control structure, you'll need to map it UC as well or use explicit
  cache flushes.

The genuine pitfall: if you ever let the CPU cache that region and don't have
a rock-solid invalidation protocol, you'll chase heisenbugs forever.

## 4. Realistic PCI bandwidth

**Theoretical:** 32-bit, 33 MHz PCI: 33 MHz × 32 bits / 8 = 133 MB/s. So your
133 MB/s figure is the classic theoretical ceiling.

**Realistic sustained:**

- With long bursts and a well-behaved target, you might see ~80–110 MB/s for
  large sequential reads.
- Instruction fetch is not large sequential DMA; it's relatively small,
  latency-sensitive reads with prefetch. Effective throughput will be lower,
  but still orders of magnitude above what you need for code density.

Your DDR3 on the FPGA being ~133 MB/s is actually the tighter bound. If
that's the internal limit, PCI won't be your bottleneck; the memory
controller on the FPGA will.

So: 133 MB/s is a fair theoretical number; expect something like 60–100 MB/s
"real" for big bursts, and don't obsess over it for instruction fetch.

## 5. In-circuit BIOS dump contention

Your diagnosis is very plausible.

- Unpowered chipset clamping the bus is a known problem.
- The MCP68's SPI pins are still physically connected; ESD/protection diodes
  and internal structures can sink or source enough current to distort the
  signal when you power only the flash.
- It's common for flashrom users to fail in-circuit on nForce/Gigabyte
  boards for exactly this reason.

Other things to sanity-check (but you sound like you already did):

- Correct VCC (3.3 V) and common ground.
- Decent local decoupling on the flash VCC when powering from the programmer.
- SPI clock not too fast (you already went down to 512 kHz).
- Clip making solid contact (you checked continuity).

**Non-destructive mitigations before desolder:**

- Lift /CS (chip select) pin of the flash to isolate it from the chipset
  while keeping the rest of the pins in-circuit.
- Or cut the /CS trace and later repair it with a bodge wire.
- Powering the board into standby and hoping the chipset stays passive is,
  as you said, risky — its own SPI master may wake up and fight you.

Given what you've tried, "chipset clamping" is the most likely culprit, not
some subtle flashrom misconfig.

## 6. Lifting only the MISO pin

Yes, that's a valid intermediate technique.

- Lifting the flash's MISO leg disconnects the chipset's input from the bus
  while your programmer still talks directly to the chip via the clip.
- It breaks the loopback path that's most likely corrupting the JEDEC ID
  reads.

**Risks:**

- Mechanical: SOIC pads on cheap boards lift easily; you need a steady hand
  and good tools.
- ESD/strain: don't flex the leg too much; you want to be able to solder it
  back down cleanly.

If you're comfortable with fine-pitch work, lifting MISO or /CS is a
perfectly sound approach and often enough to get a clean dump without full
removal.

## 7. DualBIOS recovery loop and safe workflow

This is where things get murky and undocumented.

### How DualBIOS decides to fail over

There's both hardware and firmware involvement.

- Gigabyte's marketing talks about "hardware DualBIOS", but in practice
  boards use a mix of glue logic and BIOS code to decide when to copy backup
  → main.

**Likely triggers** (from reverse-engineering and behaviour, not official docs):

- A checksum/signature failure on the main BIOS image (simple sum or
  similar) detected very early in POST.
- A watchdog that trips if POST doesn't reach a certain point (e.g., no
  successful boot after N attempts), causing the board to switch to backup
  and/or copy backup over main.
- Some boards also react to a specific key combo or jumper to force backup.

There is no public, authoritative spec for MCP68-era DualBIOS behaviour;
everything is inferred from observed behaviour and scattered
reverse-engineering.

### Safest workflow for custom BIOS

**Golden rule:** always have an external programmer and known-good dumps of
both chips. You already do, which is great.

**Concrete approach:**

- Dump both M_BIOS and B_BIOS and stash them in multiple places.
- Only modify/program M_BIOS; leave B_BIOS completely stock as your lifeline.
- Make minimal changes at first (e.g., inject a tiny POST-time payload) so
  if DualBIOS decides to be clever, you can still reason about what went
  wrong.
- If you ever get into a recovery loop where backup overwrites main, use the
  external programmer to restore your experimental image to M_BIOS and the
  original to B_BIOS.

**What I would not do initially:**

- Don't try to "disable" DualBIOS by corrupting backup or its header; if you
  get the heuristics wrong, you can end up with two bad images and a board
  that never even tries to boot.
- Don't rely on software-only flashing from within your experimental BIOS
  until you've proven you can always recover with the external programmer.
- Assume DualBIOS will occasionally decide your image is bad for opaque
  reasons and plan around that.

## 8. Award checksum (two's complement to zero)

The classic Award checksum is an 8-bit sum over the image that should wrap
to zero.

- This is primarily a BIOS-level sanity check, not a hard hardware gate.
- The CPU still fetches the reset vector from the flash; the chipset doesn't
  typically parse the image format.
- Early in POST, Award code computes the checksum and, if it fails, may
  trigger recovery logic (including DualBIOS behaviour on Gigabyte boards).

**DualBIOS may also use that checksum as a heuristic.**

- It's plausible that the DualBIOS glue logic or very early stub code checks
  the same sum and, on failure, asserts the backup chip select and/or
  initiates a copy.
- But that's an inference from behaviour, not a documented hardware spec.

So: you should absolutely keep the checksum correct if you're modifying the
image, but don't assume there's a magical hardware block that refuses to let
the CPU execute if it's wrong — the BIOS code itself is the main enforcer.

## 9. MCP68 PCI device IDs: canonical sources

NVIDIA never published a nice public MCP68 PCI ID table.

**Canonical in practice:**

- `lspci -nn` on this exact board. That gives you vendor:device IDs and
  class codes straight from reality.
- The `pci.ids` database (used by pciutils) aggregates known IDs and names.
- Linux kernel source (e.g., `drivers/ata/pata_amd.c`,
  `drivers/net/ethernet/nvidia/forcedeth.c`, etc.) contains hard-coded IDs
  for MCP61/MCP68 variants.

If you want to avoid confabulated IDs, boot anything that can run lspci (a
tiny Linux/BSD live image, even temporarily), dump the IDs, and treat that
as ground truth. Then cross-check with pci.ids and kernel drivers to
understand which function is which.

If you want, next step we can sketch a concrete memory map + MTRR/PAT setup
for your "execute from PCI BAR" scheme and a minimal handshake protocol
between ESP32 and Phenom II that doesn't rely on luck.
