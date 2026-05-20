# Verify against silicon, not against another LLM

The companion principle to [[multi-llm-consultation.md]].

The multi-LLM consultation pattern catches the *existence* of disagreements
between LLMs. It does not resolve them. Every "verification" by another
LLM is just another sample from the same kind of system — confident, fluent,
plausibly-wrong-in-the-same-way. At the level of detail this project
operates at (MSR bit layouts, SPI timing, MTRR/PAT combining rules, FPGA
PCI target behaviour), the chain of trust has to end *outside* the LLM
stack. This file lists what "outside" actually means.

## The hierarchy of evidence

In rough order of trustworthiness for this project:

1. **Direct measurement on the real rig.** A multimeter on the chip's VCC
   pin, a scope on the SPI lines, a logic analyser on the PCI bus,
   `rdmsr 0x200` from a live boot. Reality has had a vote.
2. **Read-back from the device.** Program an MTRR, then immediately
   `rdmsr` the same MSR back and verify the value matches. The CPU and
   chipset are the only authorities on what they actually accepted.
3. **Architecture manuals from the silicon vendor.** AMD64 Architecture
   Programmer's Manual (Vols 1–5) for the CPU; Macronix MX25L1605E
   datasheet for the BIOS flash; NVIDIA never publicly documented the
   MCP68, so for that chip see #4.
4. **Open-source kernel implementations that have run on the real silicon
   for years.** `arch/x86/kernel/cpu/mtrr/generic.c` in the Linux source
   has been programming K10 MTRRs correctly since the platform shipped;
   `drivers/ata/pata_amd.c` and similar files for nForce SATA;
   `pci.ids` for canonical device-ID mappings. Battle-tested code is a
   stronger source than confidently-worded prose.
5. **Multi-LLM consensus** ([[multi-llm-consultation.md]]) — points where
   3+ models independently agree. Useful evidence, *not* a verification.
   Promote claims from this level to a higher one before kernel code
   relies on them.
6. **Single LLM assertion.** Treat as a hypothesis until it has either
   survived multi-LLM cross-reference or been independently verified
   against a higher level on this list.

The temptation in a flow-state debugging session is to slide down from 1
to 6 as the question gets harder. The discipline is to slide *up* — when
something is going wrong, the answer is in the multimeter or the manual,
not in another chat window.

## Move slowly. Verify every step.

Specific applications of this principle for SINT-34 → SINT-39:

### Before flashing anything

- **Verified dumps of both M_BIOS and B_BIOS, triple-read + sha256-matched,
  out of circuit.** This is the recovery image. Do not proceed with any
  experiment that could destroy it until both files exist and have been
  copied to at least two locations off the workstation. (See
  [[dualbios-strategy.md]].)
- **`m_bios.verified.bin` and `b_bios.verified.bin` are immutable.** Every
  experiment works on a *copy*, never the original.

### Before programming MTRRs

- **Boot a Linux live USB on the real board first.** Capture `lspci -vv`,
  `cat /proc/cpuinfo`, `cat /proc/iomem`, `cat /proc/mtrr`, `dmesg`. Save
  these to the repo. These are the ground truth for: the FPGA's actual
  BAR address, PhysAddrSize, existing MTRR layout, what the BIOS has
  reserved.
- **`CPUID.80000008h:EAX[7:0]`** is the only authority for PhysAddrSize.
  Read it on the actual chip; do not assume.
- **Cross-reference MTRR programming against
  `arch/x86/kernel/cpu/mtrr/generic.c`.** That file has been correctly
  programming MTRRs on real K10 silicon for ~15 years. If your code
  computes different bit patterns than Linux does for the same region,
  your code is wrong.
- **Read back every MTRR after programming.** A `dump_mtrrs` helper that
  prints every variable-range pair after every write is a 30-line
  function and saves debugging days.

### Before executing from a PCI BAR

- **Verify the BAR is UC** by measuring read latency. UC reads against an
  MMIO BAR are dramatically slower than WB reads against system RAM. If
  the latency doesn't change after programming the MTRR, the MTRR isn't
  taking effect on that region.
- **Verify the FPGA actually decodes the address you think it does.**
  Write a known sentinel value to one offset from the ESP32, read it back
  from the Phenom. If the value matches, the BAR is plumbed correctly. If
  it doesn't, the BAR has moved, the PCI enumeration is wrong, or the
  FPGA target isn't responding.
- **Verify the failure modes are the ones you expect.** Before relying on
  `UD2` guard pages, write a test that deliberately overshoots into the
  guard region and confirm `#UD` is raised. Reality has had a vote.

### Before flashing a custom BIOS

- **Test the binary's checksum logic against a known-good Award image
  first.** If your patcher produces a different byte for the verified
  M_BIOS dump than the dump itself contains, the patcher is wrong before
  it ever touches anything experimental.
- **Flash to a *spare* MX25L1605E in the SOIC8→DIP8 socket first**, then
  read it back and `cmp` against the source image. Only after a clean
  round-trip on a spare chip do we go near the M_BIOS socket.
- **B_BIOS is physically isolated (or removed) while M_BIOS is
  experimental.** See [[dualbios-strategy.md]] §3.

## The "looks plausible" trap

Bit-level firmware code can look completely correct, compile cleanly, and
silently misconfigure the system in a way that produces only
non-reproducible weirdness weeks later. The Copilot MTRR worked example
in `research/sinter-project-assessment-copilot-followup2.md` was an
instance of exactly this: confident prose, internally consistent
arithmetic, wrong answer. The fact that *my* re-derivation in the same
file gave different numbers does not mean my re-derivation is correct —
it just means at most one of us is right.

The only resolutions are:

- AMD64 APM Vol 2 §7.7 read directly from the current PDF (not from
  recollection or chat assertion).
- Comparison against Linux source.
- `rdmsr` on the real chip after programming.

This applies to every assertion in the research notes and in the
distilled notes alike. If a claim has not yet been verified against one
of those, it's a hypothesis dressed up as a fact. Mark it as such; come
back to it when you can verify.

## Cross-reference

- [[multi-llm-consultation.md]] — the technique for *generating*
  hypotheses (and catching disagreements between LLMs).
- [[pci-bar-execute.md]] — distilled design notes for the eventual PCI
  BAR execute-in-place architecture.
- [[dualbios-strategy.md]] — distilled BIOS-side strategy.
- `research/` — verbatim LLM transcripts, with disputed claims flagged
  in their respective preambles.
