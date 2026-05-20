# Sinter project assessment — Microsoft Copilot (follow-up 2)

Copilot's response to the three follow-up asks (failure-mode catalogue, AMD K10
MSR cheat-sheet, x86 cache/ordering primitives table). Pasted into the
conversation as plain text; preserved verbatim below for traceability.

**Status: not folded into the distilled notes.** Same reason as
`-followup.md` — written before SINT-34 / `lspci` / FPGA are in hand. Two
specific concerns when this is eventually mined for the kernel work:

## ⚠ The worked MTRR example in §2.8 contains arithmetic errors

The bit-layout descriptions earlier in §2 are correct, but the worked example
that follows them is wrong in two reinforcing ways. Anyone following the
example verbatim would program the wrong physical address into the MTRR.

For a 16 MiB UC region at physical `0xF0000000` on a 36-bit phys-addr K10:

| Field | Copilot's value | Correct value |
|---|---|---|
| `IA32_MTRRphysBase0` (full MSR value) | `0x00000000_000F0000` | `0x00000000_F0000000` |
| `IA32_MTRRphysMask0` (full MSR value) | `0x00000000_000FF800` | `0x0000000F_FF000800` |

Two compounding errors:

1. **MSR value vs field value.** The base/mask fields live at MSR bit
   positions `[PA-1:12]`. The MSR's bit 12 corresponds to phys-addr bit 12 —
   so the *field value* `BASE >> 12` must be **placed back** at bit position
   12 in the final 64-bit MSR write, not written out as the low bits. The
   correct base value is the physical address itself (`0xF0000000`) with
   `TYPE` in the low 8 bits.
2. **32-bit arithmetic on a 36-bit phys-addr.** `~0x00FFFFFF` is `0xFF000000`
   in 32-bit C, but should be `0xFFF000000` in 36-bit phys-addr math. So
   the mask field has one fewer `F` than it should, and the region ends up
   covering 256 MiB rather than 16 MiB *if* the bit-position error above is
   independently corrected.

The recipe to get this right by hand:

```
PhysBase MSR value = (BASE & PhysAddrMask) | TYPE
                   = 0xF0000000 | 0x00  = 0x00000000_F0000000

PhysMask MSR value = (~(SIZE - 1) & PhysAddrMask) | V_BIT
                   = (~(0x01000000 - 1) & 0xFFFFFFFFF) | 0x800
                   = 0x0000000F_FF000000 | 0x800
                   = 0x0000000F_FF000800
```

Where `PhysAddrMask` is `(1 << PhysAddrSize) - 1` — verify the actual
`PhysAddrSize` from CPUID 0x80000008 on the real chip; K10 is commonly 40
bits for the C0/C3 steppings, which would extend the mask further left
again. Don't trust the "36" assumption without checking.

## ⚠ The "compute the effective memory type" advice in §1.6/§1.15

The MTRR/PAT effective-type computation is genuinely subtle: when MTRR and
PAT disagree, AMD's combining rules apply, and they're *not* a straight
"strictest wins" — UC + WB combines to UC, but WC + WB combines to *WC*, not
WB. Implementing your own "compute effective type" helper as Copilot
suggests is the right *idea* but needs the actual K10 combining table, not
intuition. Reference: AMD64 APM Vol 2, "Combined Memory Types" section.

## What's worth folding in *after* reality has voted

From the failure-mode catalogue (§1):

- **1.5 BAR moved by PCI re-enumeration.** Genuinely missing from the
  distilled notes. Rule: re-enumerate PCI on every boot, reprogram MTRRs
  against the *current* BAR address. Never hard-code.
- **1.10 CPU hangs on read to unresponsive MMIO.** The FPGA's PCI target IP
  must respond to all addresses in the BAR window, even with dummy data,
  to avoid wedging the bus on an unmapped offset.
- **1.13 Posted-write reordering inside the FPGA.** Internal write
  reordering inside the FPGA between control-block writes and code writes
  is a real failure mode separate from the chipset's posted-write buffer;
  worth addressing in the FPGA target design.

From the fences table (§3): no folding needed — confirms the existing
`pci-bar-execute.md` §3 serialisation guidance and the explicit warning
against `invd`.

---

I love how seriously you're taking the failure modes—that's exactly what
keeps this kind of project from turning into superstition. I'll treat each
of your three asks as its own artifact.

## 1. Failure‑mode catalogue for "execute from PCI BAR with async ESP32 writer"

Below is a grab‑bag you can grep mentally: symptom → root cause →
diagnostic → fix. Many of these overlap; that's the point—real bugs often
look like two or three of these at once.

### 1.1 Stale instruction fetch (CPU executes old code after ESP32 update)

**Symptom:**
- Intermittent wrong behaviour: "old" code still runs after ESP32 claims to
  have updated the region.
- Happens more often after rapid successive updates; rarely reproducible
  with long delays.

**Root cause:**
- BAR region mapped cacheable (WB/WT/WC) in MTRR/PAT.
- CPU fetched and cached instructions from the region; ESP32 wrote new
  code, but CPU executes from stale I‑cache / unified cache lines.

**Diagnostic:**
- Dump MTRR and PAT configuration and verify effective memory type for the
  BAR.
- Simple test:
  - ESP32 writes a pattern of NOPs followed by UD2 (illegal instruction).
  - CPU jumps into region and should trap on UD2.
  - Then ESP32 overwrites with a different pattern (e.g., NOP; NOP; RET).
  - If CPU still traps on UD2 sometimes, you're executing stale code.

**Fix:**
- Mark the entire BAR as UC via variable‑range MTRR.
- Ensure page tables don't override it to a cacheable type via PAT.
- Reboot and re‑verify with the same test.

### 1.2 Descriptor seen mid‑update (torn control block)

**Symptom:**
- CPU occasionally jumps to obviously bogus addresses (e.g., offset
  `0x00000004`, or into the control block itself).
- Crashes are non‑deterministic and correlate with high update frequency
  from ESP32.

**Root cause:**
- ESP32 writes descriptor fields in multiple bus transactions.
- CPU reads descriptor while ESP32 is mid‑update: `code_addr` from new
  descriptor, `seq` from old, or vice versa.
- No versioning or "write seq last" discipline.

**Diagnostic:**
- Instrument CPU side to log every descriptor it sees before jumping:
  `(seq, addr, len, flags)`.
- Look for impossible combinations (e.g., `len = 0`, `addr` outside BAR,
  `seq` jumping backwards).
- On FPGA side, capture bus transactions to confirm descriptor writes are
  multi‑beat and not atomic.

**Fix:**
- Use a versioned descriptor:
  - ESP32 writes all fields except `seq`.
  - Memory barrier.
  - ESP32 writes `seq` last.
- CPU reads `seq` twice around the descriptor and only accepts it if both
  reads match and differ from last seen.

### 1.3 Racing the descriptor write (CPU jumps before code fully written)

**Symptom:**
- CPU jumps into region and executes partially updated code:
  - Sometimes traps on invalid opcode.
  - Sometimes runs but produces nonsense.
- Reproducible when ESP32 is slow or code size is large.

**Root cause:**
- ESP32 writes code and descriptor in the wrong order: sets "ready" flag
  or increments `seq` before all code bytes are committed to DDR3.
- Or: FPGA's internal write buffering reorders descriptor write ahead of
  some code writes.

**Diagnostic:**
- On FPGA, add a debug mode where it logs the last N writes (address,
  data, timestamp).
- Confirm whether descriptor write (control block) appears before the last
  code write.
- On CPU, add a "self‑check" at entry: compute checksum over code region
  and compare with descriptor's checksum; log mismatches.

**Fix:**
- Enforce strict ordering on ESP32 side:
  - Write code region.
  - Ensure all writes are flushed to FPGA (ESP32 memory barrier + any
    bus‑specific flush).
  - Write descriptor fields.
  - Memory barrier.
  - Write `seq` last.
- On FPGA, ensure descriptor writes are not reordered ahead of code writes
  (e.g., separate write queues or explicit ordering).

### 1.4 Prefetch wandering into non‑code / control block

**Symptom:**
- CPU occasionally traps on UD2 or invalid opcode in what should be a
  data/control area.
- Or: CPU executes random garbage that looks like data, not code.
- Often triggered by code near the end of a region.

**Root cause:**
- CPU instruction prefetch crosses your logical boundary between "code"
  and "data/control".
- Control block or other data is adjacent to executable region in the BAR.
- Prefetch doesn't respect your semantic boundaries.

**Diagnostic:**
- Place a guard page after the code region filled with UD2.
- If you see traps at addresses just past the intended code end, you're
  seeing prefetch or fall‑through.
- Move control block further away and see if the symptom disappears.

**Fix:**
- Align code regions to page boundaries (4 KiB or 64 KiB).
- Place at least one page of non‑executable guard (UD2) after each code
  region.
- Place control block and data well away from executable regions.

### 1.5 BAR moved by PCI re‑enumeration

**Symptom:**
- System works after cold boot, but after a warm reboot or after
  adding/removing PCI devices, CPU jumps into what used to be the BAR and
  now is some other device's MMIO or unmapped space.
- Symptoms: hangs, machine checks, or weird device behaviour.

**Root cause:**
- BIOS or your own PCI enumeration code reassigns BARs.
- You cached the BAR physical address once and never updated it.
- On subsequent boots or after hot‑plug changes, the FPGA BAR moves.

**Diagnostic:**
- Log the BAR address at each boot from PCI config space.
- Compare across boots and configurations.
- If it changes, you're relying on a stale address.

**Fix:**
- On every boot, re‑enumerate PCI and read the BAR address from config
  space.
- Reprogram MTRRs and page tables based on the current BAR location.
- Never hard‑code the BAR address.

### 1.6 Inconsistent MTRR/PAT configuration (effective type not UC)

**Symptom:**
- Behaviour suggests caching (stale instructions, inconsistent reads), but
  MTRR dump "looks right" at first glance.
- Only certain sub‑ranges misbehave.

**Root cause:**
- Overlapping MTRRs with different types; effective type is not what you
  think.
- PAT overrides page type to something cacheable.
- Fixed‑range MTRRs cover low memory and interact with variable ranges in
  unexpected ways.

**Diagnostic:**
- For a given physical address in the BAR, compute the effective memory
  type:
  - Check all variable MTRRs that cover it.
  - Check default type.
  - Check PAT entry used by the page table.
- Write a small kernel function that, given a virtual address, prints the
  effective type based on your own logic; cross‑check with observed
  behaviour.

**Fix:**
- Use a single variable‑range MTRR for the BAR, aligned and sized
  correctly.
- Ensure no other MTRR overlaps that region.
- Use a PAT entry that maps to UC for the pages covering the BAR.
- Keep fixed‑range MTRRs at defaults unless you really need to touch them.

### 1.7 FPGA bus glitches / marginal timing

**Symptom:**
- Rare, non‑reproducible crashes: invalid opcodes, random jumps, or silent
  data corruption.
- More frequent at higher PCI clock or when other PCI devices are active.
- Logic analyser shows occasional malformed transactions.

**Root cause:**
- FPGA PCI interface marginal: setup/hold violations, metastability, or
  incorrect handling of wait states.
- DDR3 controller inside FPGA occasionally returns corrupted data under
  load.

**Diagnostic:**
- Use a PCI bus analyser or logic analyser on the PCI signals.
- Run a stress test: CPU reads large blocks from the BAR and checks them
  against known patterns written by ESP32.
- If you see bit flips or inconsistent reads with no pattern, suspect
  signal integrity or timing.

**Fix:**
- Tighten FPGA timing constraints; ensure PCI interface meets spec.
- Add internal ECC or parity on DDR3 if possible.
- Reduce PCI clock or simplify the design (e.g., fewer outstanding reads).
- Improve board‑level signal integrity (shorter traces, better
  termination).

### 1.8 ESP32 missing a memory barrier

**Symptom:**
- Descriptor fields sometimes appear "out of order" from CPU's perspective:
  - `seq` updated but `code_addr` still old.
  - Or checksum mismatches sporadically.
- More frequent under high ESP32 load or when Wi‑Fi/other peripherals are
  active.

**Root cause:**
- ESP32's memory system reorders writes to DDR3‑mapped FPGA region.
- Without an explicit barrier, descriptor write can be visible before some
  code writes.

**Diagnostic:**
- On ESP32, add instrumentation: after writing code and descriptor, read
  back the region and verify it matches what you intended.
- If read‑back shows correct data but CPU still sees torn descriptors,
  suspect bus ordering between ESP32 and FPGA.
- If read‑back itself shows out‑of‑order or partial writes, you're missing
  barriers.

**Fix:**
- Use ESP32's memory barrier primitives (e.g., `__sync_synchronize()` or
  platform‑specific fence) between code writes and descriptor writes.
- If the ESP32 talks to FPGA over a bus with its own ordering rules (e.g.,
  SPI, parallel bus), ensure you respect those (e.g., wait for transaction
  completion).

### 1.9 Partial cache‑line tearing (if BAR accidentally cacheable)

**Symptom:**
- CPU reads a 16‑byte instruction sequence where half is old, half is new.
- Leads to invalid opcodes or bizarre behaviour that doesn't match either
  old or new code.
- Very rare and extremely hard to reproduce.

**Root cause:**
- BAR mapped cacheable.
- ESP32 writes into the middle of a cache line that the CPU has already
  fetched.
- CPU later refills the line partially or merges writes in a way that
  produces a hybrid line.

**Diagnostic:**
- Only really diagnosable by:
  - Forcing cacheable mapping intentionally in a test environment.
  - Logging instruction bytes at the faulting address and comparing them
    to both old and new code images.
  - If you see a mixture, you've hit tearing.

**Fix:**
- Same as stale instruction fix: never cache the BAR.
- UC mapping eliminates this class of bug.

### 1.10 CPU hangs hard, NMI doesn't help

**Symptom:**
- System freezes; NMI handler never runs.
- No triple fault, no reboot, just a dead machine.
- Often happens when jumping into obviously invalid addresses.

**Root cause:**
- Jump into MMIO space that doesn't respond properly (e.g., no device, or
  device that never returns data).
- CPU waits forever on a bus transaction that never completes.
- Or: you jumped into an address that hits some chipset‑reserved region
  that wedges the bus.

**Diagnostic:**
- Log the target address before every jump into BAR.
- If the last logged address is outside the FPGA BAR or in a weird range,
  that's your culprit.
- Use PCI config space to confirm BAR size and decode range.
- If you have a bus analyser, check whether the read to that address ever
  completes.

**Fix:**
- Add range checks before jumping: ensure target address is within the
  known BAR range and within a valid code region.
- Ensure FPGA responds to all addresses in the BAR (even if with dummy
  data) to avoid hanging the bus.
- Avoid mapping other devices' MMIO near your BAR in early experiments.

### 1.11 Double‑faults on entry to BAR

**Symptom:**
- CPU triple‑faults and reboots (or halts) immediately after jumping into
  BAR.
- Sometimes you see a `#GP` or `#PF` first, then double‑fault.

**Root cause:**
- Code in BAR references invalid segment selectors, invalid stack, or
  unmapped data pages.
- Or: IDT/GDT not set up correctly before jumping into BAR code.
- Or: BAR region not mapped in page tables (if paging enabled).

**Diagnostic:**
- Temporarily install a very simple IDT with a handler for `#GP` and `#PF`
  that logs the faulting RIP and error code to a serial port or memory.
- If the faulting RIP is inside the BAR, inspect the instruction and its
  operands.
- Check page tables to ensure the BAR region is mapped with correct
  permissions.

**Fix:**
- Ensure your "BAR code" is self‑contained or uses only well‑defined
  segments and stacks.
- Map the BAR region in page tables before jumping.
- Keep early BAR experiments in flat 1:1 mapping with simple segments.

### 1.12 Wrong region executed (off‑by‑one or stale region index)

**Symptom:**
- CPU executes valid code, but it's the wrong version or wrong region
  (e.g., Region A instead of B).
- Behaviour is consistent but not what you expect.

**Root cause:**
- Bug in your region selection logic:
  - Using stale region index.
  - Misinterpreting descriptor fields.
  - Off‑by‑one in base address calculation.

**Diagnostic:**
- Add a unique signature at the start of each region (e.g., write a known
  constant to a debug port or memory location).
- When code runs, log which signature you see.
- Compare with what the ESP32 intended to activate.

**Fix:**
- Simplify region selection logic.
- Use explicit base addresses for Region A and B, not computed offsets
  that can overflow.
- Validate descriptor fields before use.

### 1.13 Posted‑write reordering inside FPGA

**Symptom:**
- CPU sees descriptor update before some code bytes are actually visible,
  even though ESP32 did the right thing.
- Similar to "racing descriptor write", but root cause is inside FPGA.

**Root cause:**
- FPGA internally buffers writes from ESP32 and reorders them for DDR3
  efficiency.
- Control block writes (small, aligned) may be committed earlier than
  large code writes.

**Diagnostic:**
- Add internal logging in FPGA:
  - For each incoming write, log `(addr, data, time)`.
  - For each DDR3 commit, log `(addr, data, time)`.
- Compare ordering of descriptor vs code writes.
- If descriptor commits earlier, you've found it.

**Fix:**
- Implement write ordering rules in FPGA:
  - Treat control block region as "strongly ordered": no reordering across
    it.
  - Or: require ESP32 to explicitly flush a write queue before writing
    descriptor.
- Alternatively, have CPU poll a "DDR3 flush complete" flag set by FPGA
  after all writes are committed.

### 1.14 Mis‑programmed MTRR (wrong base/size encoding)

**Symptom:**
- Some addresses in the BAR behave UC, others behave cacheable.
- Bugs appear only when executing from certain offsets.

**Root cause:**
- MTRR base/size encoding wrong:
  - Base not aligned to size.
  - Mask bits mis‑set, so region is smaller or larger than intended.
  - Parts of the BAR fall outside the UC region.

**Diagnostic:**
- For a set of addresses across the BAR, run a small test:
  - Measure read latency (UC vs WB is very different).
  - Or, intentionally write self‑modifying code and see if it behaves like
    cached memory.
- Compare with your MTRR configuration and recompute the actual covered
  range.

**Fix:**
- Recalculate base and mask:
  - Base aligned to region size.
  - Mask encodes size as per AMD docs (high bits 1, low bits 0).
- Use a single MTRR pair that exactly covers the BAR.

### 1.15 Wrong PAT entry used (page tables override UC)

**Symptom:**
- MTRR says UC, but behaviour looks cacheable.
- Only some pages misbehave.

**Root cause:**
- Page table entries use a PAT index that maps to WB or WT.
- Effective type is the combination of MTRR and PAT; PAT can override.

**Diagnostic:**
- Dump page tables for the BAR region and inspect PTE bits (PWT, PCD, PAT).
- Dump PAT MSR and see what each index maps to.
- Compute effective type for a few addresses and compare with observed
  behaviour.

**Fix:**
- Use a PAT entry that maps to UC for all pages in the BAR.
- Or set PCD/PWT bits in PTEs to force UC.
- Keep PAT configuration simple: one entry for WB, one for UC, avoid fancy
  mixes.

You can extend this catalogue as you discover real bugs—just keep the
pattern: symptom → root cause → diagnostic → fix.

## 2. AMD K10 cache/MTRR/PAT MSR cheat‑sheet

I'll focus on what you actually need for mapping a UC MMIO region on
Phenom II. Names follow Intel where possible, but I'll note AMD quirks.

**Note:** MSR numbers are the same as Intel's for MTRR/PAT; AMD mirrors
them for compatibility.

### 2.1 IA32_MTRRcap (MSR 0xFE)

**Purpose:** Reports MTRR capabilities: number of variable ranges,
fixed‑range support, write‑combining support.

**Layout (bits):**
- `[7:0]` VCNT – Number of variable‑range MTRR pairs (base/mask).
- `[8]` FIX – 1 if fixed‑range MTRRs are supported.
- `[9]` WC – 1 if write‑combining type is supported.
- `[63:10]` Reserved.

**Use:**
- Read once at boot to know how many variable MTRRs you can use.
- On Phenom II, VCNT is typically 8.

### 2.2 IA32_MTRR_DEF_TYPE (MSR 0x2FF)

**Purpose:** Controls default memory type and global MTRR enable.

**Layout:**
- `[7:0]` TYPE – Default memory type (e.g., 0 = UC, 6 = WB).
- `[9:8]` Reserved.
- `[10]` FE – Fixed‑range MTRRs enable (1 = enabled).
- `[11]` E – MTRRs enable (1 = enabled).
- `[63:12]` Reserved.

**Use:**
- Typically: TYPE = WB, FE = 1, E = 1.
- When reprogramming MTRRs, you usually:
  - Disable caching (`CR0.CD=1`, `CR0.NW=0`, `WBINVD`).
  - Clear E (disable MTRRs).
  - Program variable MTRRs.
  - Set E (enable MTRRs).
  - Re‑enable caching.

### 2.3 Variable‑range MTRRs

Each pair:
- `IA32_MTRRphysBaseN` – MSR `0x200 + 2*N`
- `IA32_MTRRphysMaskN` – MSR `0x201 + 2*N`
- N = 0..(VCNT‑1).

**`IA32_MTRRphysBaseN` layout:**
- `[7:0]` TYPE – Memory type for this range (0=UC, 1=WC, 4=WT, 5=WP, 6=WB).
- `[11:8]` Reserved.
- `[M-1:12]` BASE – Physical base address bits `[M-1:12]`, where M is max
  phys addr width (e.g., 36 or 40).
- `[63:M]` Reserved.

**`IA32_MTRRphysMaskN` layout:**
- `[10:0]` Reserved.
- `[11]` V – Valid bit (1 = this MTRR pair is active).
- `[M-1:12]` MASK – Address mask; encodes size.
- `[63:M]` Reserved.

**Size encoding:**
- Region size = `2^N` bytes, where N is the number of low‑order zero bits
  in MASK (starting at bit 12).
- BASE must be aligned to region size.

**Use:**
- For a 16 MiB region at `0xF0000000`:
  - BASE = `0xF0000000 >> 12 = 0xF0000`.
  - MASK = `~(size-1) >> 12 = ~(0x01000000-1) >> 12 = 0xFFF00000 >> 12 = 0xFFF00`.
  - TYPE = 0 (UC).
  - V = 1.
- (Worked example at the end.)

### 2.4 Fixed‑range MTRRs

**MSRs:**
- `IA32_MTRR_FIX64K_00000` – `0x250`
- `IA32_MTRR_FIX16K_80000` – `0x258`
- `IA32_MTRR_FIX16K_A0000` – `0x259`
- `IA32_MTRR_FIX4K_C0000` – `0x268`
- `IA32_MTRR_FIX4K_C8000` – `0x269`
- `IA32_MTRR_FIX4K_D0000` – `0x26A`
- `IA32_MTRR_FIX4K_D8000` – `0x26B`
- `IA32_MTRR_FIX4K_E0000` – `0x26C`
- `IA32_MTRR_FIX4K_E8000` – `0x26D`
- `IA32_MTRR_FIX4K_F0000` – `0x26E`
- `IA32_MTRR_FIX4K_F8000` – `0x26F`

Each fixed MTRR MSR covers a fixed physical range (e.g., `0x00000-0x7FFFF`,
`0x80000-0x9FFFF`, etc.) and contains multiple 8‑bit type fields.

**Layout (example for 64K):**
- 8 bytes, each byte = memory type for an 8 KiB sub‑range.
- So `8 * 8 KiB = 64 KiB`.

**Use:**
- Mostly for low memory (BIOS, legacy).
- For your BAR at `0xF0000000`, fixed MTRRs are irrelevant; just leave them
  at defaults.

### 2.5 IA32_PAT (MSR 0x277)

**Purpose:** Defines 8 memory types used by PAT index bits in PTEs.

**Layout:**
- 8 entries, each 8 bits:
  - `[2:0]` MT – Memory type (same encoding as MTRR TYPE).
  - `[7:3]` Reserved.
- So:
  - Bits `[7:0]` – PAT0
  - `[15:8]` – PAT1
  - …
  - `[63:56]` – PAT7

**Use:**
- Combine with PTE bits (PWT, PCD, PAT) to select one of 8 PAT entries.
- For simplicity, you can set:
  - PAT0 = WB
  - PAT1 = WC
  - PAT2 = UC
  - etc.
- Then ensure pages mapping the BAR use the PAT index that selects UC.

### 2.6 MSR_HWCR (Hardware Configuration, AMD‑specific)

**MSR:** `MSR_HWCR – 0xC0010015`

**Relevant bits (for cache control):**
- `[0]` SMMLOCK – SMM lock (not relevant here).
- `[1]` TLB_CACHE_DIS – Disable TLB cache (don't touch unless you know
  why).
- `[3]` INVD_WBINVD – Controls behaviour of INVD/WBINVD
  (implementation‑specific).
- Other bits control things like instruction cache disable, etc.,
  depending on stepping.

**Use:**
- For your purposes, you generally don't need to touch MSR_HWCR.
- Stick to `CR0.CD/NW` and MTRRs for cache control.

### 2.7 Safe MTRR programming sequence (K10‑compatible)

**Goal:** Change variable MTRRs without corrupting caches or seeing
transient inconsistent types.

**Typical sequence:**

1. Disable interrupts.
2. Disable caching:
   - Read CR0.
   - Set `CR0.CD = 1` (cache disable), `CR0.NW = 0`.
   - Write CR0.
   - Execute `WBINVD` to flush caches.
3. Disable MTRRs:
   - Read `IA32_MTRR_DEF_TYPE`.
   - Clear bit E (bit 11).
   - Write back.
4. Program variable MTRRs:
   - For the chosen MTRR pair:
     - Write `IA32_MTRRphysBaseN` with TYPE and BASE.
     - Write `IA32_MTRRphysMaskN` with MASK and V=1.
   - Ensure all other MTRRs are either valid and correct or V=0.
5. Re‑enable MTRRs:
   - Read `IA32_MTRR_DEF_TYPE`.
   - Set bit E (and FE if you use fixed ranges).
   - Write back.
6. Re‑enable caching:
   - Read CR0.
   - Clear `CR0.CD` (0), ensure `CR0.NW=0`.
   - Write CR0.
   - Optionally execute `WBINVD` again.
7. Re‑enable interrupts.

On a uniprocessor system this is enough; on SMP you need to coordinate
across cores, but you're likely on a single core for this project.

### 2.8 Worked example: 16 MiB UC region at 0xF0000000

**⚠ See the warning at the top of this file — the MSR values in this
example are wrong both in field-vs-MSR positioning and in 32-bit-vs-36-bit
arithmetic. Correct values are given in the warning section.**

Assume:
- Physical address width = 36 bits (common).
- Region: `0xF0000000-0xF0FFFFFF` (16 MiB).
- Use MTRR pair 0.

**Step 1: Compute BASE and MASK**

- Region size = 16 MiB = `0x01000000`.
- BASE = `0xF0000000`.
- BASE field = `BASE >> 12 = 0xF0000000 >> 12 = 0x000F0000`.
- MASK = `~(size-1) = ~(0x01000000-1) = ~0x00FFFFFF = 0xFF000000`.
- MASK field = `MASK >> 12 = 0xFF000000 >> 12 = 0x000FF000`.

**`IA32_MTRRphysBase0` (`0x200`):**
- TYPE = 0 (UC) → bits `[7:0]` = `0x00`.
- BASE field in bits `[M-1:12]` = `0x000F0000`.
- So the 64‑bit value:
  - Bits `[7:0]` = `0x00`.
  - Bits `[11:8]` = 0.
  - Bits `[35:12]` = `0x000F0000`.
  - Higher bits 0.
- In hex (conceptually):
  - `MTRRphysBase0 = 0x00000000_000F0000` (TYPE=0, BASE=`0xF0000000`).

**`IA32_MTRRphysMask0` (`0x201`):**
- MASK field = `0x000FF000` in bits `[35:12]`.
- V = 1 in bit 11.
- So:
  - Bits `[10:0]` = 0 except bit 11 = 1.
  - Bits `[35:12]` = `0x000FF000`.
  - Higher bits 0.
- In hex (conceptually):
  - `MTRRphysMask0 = 0x00000000_000FF800`
  - (Exact low bits depend on how you pack V and MASK; the key is MASK
    field = `0x000FF000` and V=1.)

**Sequence:**
- Disable caching + MTRRs as above.
- Write `MTRRphysBase0 = 0x00000000000F0000`.
- Write `MTRRphysMask0 = 0x00000000000FF800` (MASK + V).
- Re‑enable MTRRs and caching.

Then map the BAR pages with a PAT entry that corresponds to UC (or set
`PCD/PWT` appropriately).

## 3. x86 cache/memory‑ordering/serialisation primitives compared

Here's a focused table. Assume Phenom II (no `serialize` instruction;
that's newer Intel).

### 3.1 Comparison table

| Instruction | What it does | What it does not do | Ordering/serialisation properties | Architecturally serialising? | Cost (rough) | Use when… | Don't use when… |
|---|---|---|---|---|---|---|---|
| `mfence` | Orders all prior loads/stores before all subsequent loads/stores. | Does not flush caches or TLBs; does not sync instruction stream. | Full memory fence for both reads and writes. | No | Tens of cycles | You need strict ordering of data accesses across cores/agents. | You think it will make new code visible to I‑cache or flush caches. |
| `lfence` | Orders all prior loads before subsequent loads. | Does not order stores; does not flush caches; no instruction serialise. | Load‑load fence (and often used as speculation barrier). | No | Tens of cycles | You need to ensure loads before lfence are globally visible/retired before later loads. | You expect it to flush or synchronise instruction fetch. |
| `sfence` | Orders all prior stores before subsequent stores. | Does not order loads; does not flush caches. | Store‑store fence. | No | Tens of cycles | You need to ensure all prior writes reach visibility before later writes (e.g., MMIO). | You expect it to affect instruction stream or read ordering. |
| `wbinvd` | Writes back and invalidates entire cache hierarchy. | Does not guarantee anything about external devices' caches. | Acts as a serialising instruction; all prior memory ops complete, caches flushed. | Yes | Thousands of cycles | You need to flush all caches (e.g., before changing MTRRs, or rare global coherency reset). | In normal code paths; inside tight loops; as a "fix" for protocol bugs. |
| `invd` | Invalidates caches without writeback (discard dirty data). | Does not preserve memory contents; can corrupt memory. | Serialising; all prior ops complete, then caches invalidated without writeback. | Yes | Thousands of cycles | Almost never; only in very controlled low‑level firmware scenarios. | Anywhere you care about memory correctness; as a "fast wbinvd". |
| `clflush` | Flushes a specific cache line (by address) from all cache levels. | Does not order other memory ops; not a full fence. | Ensures line is written back and invalidated; ordering only wrt that line. | No (but strongly ordered) | Hundreds of cycles per line | You need to evict a specific line (e.g., D‑cache coherency with DMA). | As a general "make code visible" tool when region is UC; in hot paths over large regions. |
| `clflushopt` | Optimised clflush; may be more weakly ordered, better throughput. | Same as clflush: not a full fence; doesn't serialise instruction stream. | Similar to clflush but allows more reordering; often paired with sfence for ordering. | No | Lower per‑line than clflush | High‑throughput flushing of many lines (if supported). | Assuming it's present on Phenom II (it isn't); relying on it for strict ordering. |
| `clwb` | Writes back a cache line without invalidating it. | Does not invalidate; not a full fence. | Ensures data reaches memory; ordering via sfence/mfence as needed. | No | Similar to clflushopt | Persistent memory scenarios (not relevant on Phenom II). | On hardware that doesn't support it (Phenom II); as a generic flush. |
| `cpuid` | Serialises execution; all prior instructions complete before it retires. | Does not flush caches; not a memory fence per se (but acts like one). | Full serialising instruction; orders both instruction and data streams. | Yes | Hundreds of cycles | You need a simple, portable serialisation point (e.g., after changing CR0, MTRRs, etc.). | In hot paths; as a replacement for proper fences in multi‑core synchronisation. |
| `serialize` | Newer Intel instruction: full serialisation of instruction stream. | Not present on Phenom II. | Full serialisation; stronger than fences. | Yes | High | On CPUs that support it, instead of cpuid for serialisation. | On K10/Phenom II (it doesn't exist). |

### 3.2 Key distinctions you care about

**Memory ordering vs instruction‑stream serialisation**

Fences (`mfence`/`lfence`/`sfence`):
- Control ordering of memory operations as seen by other cores/devices.
- They do not guarantee that the instruction stream is refetched or that
  I‑cache sees new code.
- They do not flush caches.

Serialising instructions (`cpuid`, `wbinvd`, `invd`, control register
writes, etc.):
- Ensure all prior instructions complete and all side effects are visible
  before subsequent instructions execute.
- Often used when changing global CPU state (CR0, CR3, MTRRs).

**For your UC‑mapped BAR:**
- You do not need fences or serialising instructions to make new code
  visible to I‑cache, because UC means the CPU fetches directly from
  memory each time.
- You do need ordering on the writer side (ESP32 + FPGA) so that code
  bytes are written before the descriptor/seq.

**`invd` vs `wbinvd`**

`wbinvd`:
- Writes back dirty cache lines to memory, then invalidates them.
- Safe for memory correctness; just slow.
- Use when you must ensure memory and caches are coherent (e.g., after
  changing MTRRs).

`invd`:
- Invalidates caches without writing back dirty data.
- Any modified data in cache is lost; memory may be stale.
- Footgun unless you are in a very controlled environment where you know
  caches don't contain important dirty data.

For your project:
- Use `wbinvd` in the MTRR programming sequence.
- Never use `invd` unless you're doing something extremely specific in
  early firmware and know exactly what's dirty.

**What you actually need before jumping into freshly written code in a UC region**

Given:
- BAR region is mapped UC via MTRR/PAT.
- CPU does not cache instructions from that region.

Then:
- On the CPU side, you do not need `clflush`, `mfence`, `cpuid`, etc., to
  see new code. Each fetch goes to memory.
- On the writer side (ESP32 + FPGA), you must ensure:
  - All code writes are committed to DDR3.
  - Descriptor/seq is written after code.
  - Any internal write buffers are flushed.

On the CPU side, you only need:
- A simple polling loop that reads `seq` and descriptor in a consistent
  way (read `seq`, read descriptor, read `seq` again).
- Optional: a memory barrier (`mfence`) between reads if you're paranoid,
  but with UC mapping even that is mostly belt‑and‑braces.

If you ever experiment with caching the BAR (for performance), then the
story changes dramatically:
- You'd need `clflush` or `wbinvd` to evict stale lines.
- You'd need a serialising instruction (`cpuid`) after flushes before
  executing.
- You'd need a much stricter protocol between ESP32 and CPU.

For now, the safest and sanest path is: UC mapping + no CPU‑side cache
tricks + strong writer‑side ordering.

If you want, next round we can turn this into concrete code: MTRR setup
routines, a minimal descriptor protocol implementation, and a tiny "hello
from BAR" test harness that either works or fails in very specific,
debuggable ways.
