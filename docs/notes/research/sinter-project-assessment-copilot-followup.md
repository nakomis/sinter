# Sinter project assessment — Microsoft Copilot (follow-up)

Pasted into the conversation as plain text after Copilot offered to "sketch a
concrete memory map + MTRR/PAT setup + handshake protocol". Preserved
verbatim below for traceability.

**Status: not folded into the distilled notes.** Per the "reality has had a
vote" rule, this is paper architecture written before we have:

- a verified BIOS dump (SINT-34 blocker),
- `lspci -vv` output from the real board,
- the Tang Primer 20K with a working PCI target IP claiming a BAR.

Two specific bits in here are worth folding into `pci-bar-execute.md`
*after* those three artefacts are in hand:

1. **Non-prefetchable BAR flag** in PCI config space — explicitly called out
   for the first time across the four LLM assessments. Prefetchable BARs let
   the chipset issue speculative reads, which interacts poorly with
   ordering-sensitive reads of the control block. Worth setting per-region.
2. **Stable-seq polling pattern** — read `seq`, barrier, read fields,
   barrier, read `seq` again; only act if both reads match. Slightly more
   robust than the single-read pattern already in
   `pci-bar-execute.md` §3 (A/B double-buffering subsection).

Everything else in here is a restatement of patterns already documented.

---

Absolutely — let's go deeper and make this concrete.
Below is a structured, technically grounded expansion of the
execute-from-PCI-BAR architecture, including memory map, MTRR/PAT setup, and
a robust ESP32 ↔ Phenom II handshake protocol that avoids undefined
behaviour.

I'll keep the tone direct and engineering-honest, because this is the kind of
project where hand-waving gets you killed.

## 🧩 Memory Map for "Execute From PCI BAR"

The Phenom II sees your FPGA as a memory-mapped PCI device. The BAR will
appear somewhere in the physical address space, typically above 0xC0000000
or in the 0xE0000000–0xFEBFFFFF region depending on how the BIOS allocates
MMIO.

A realistic layout:

```
0x00000000 – 0x0009FFFF   Low RAM
0x000A0000 – 0x000FFFFF   Legacy holes
0x00100000 – <top of RAM> System RAM
<ACPI/PCI windows>
0xE0000000 – 0xEFFFFFFF   PCI MMIO space (typical)
0xF0000000 – 0xF0FFFFFF   Your FPGA BAR (example)
0xFEC00000 – 0xFEC00FFF   IO-APIC
0xFED00000 – 0xFED003FF   HPET (if present)
```

Your BAR might be 16–64 MiB. Inside that region, you want:

```
[ Code Region A ]   (exec)
[ Code Region B ]   (exec)
[ Control Block ]   (UC, polled)
[ Guard Page ]      (filled with UD2)
```

The guard page is important because the CPU will prefetch past your intended
code boundaries.

## 🔧 MTRR + PAT Setup (Phenom II)

**Goal:** Make the BAR uncacheable so the CPU never executes stale
instructions written by the ESP32.

**Why UC?** Because x86's self-modifying-code rules only apply when the CPU
itself performs the writes. External DMA/FPGA/ESP32 writes are invisible to
the caches.

**Correct approach:**

- Use MTRRs to mark the entire BAR as UC.
- Phenom II supports variable-range MTRRs.
- Align the BAR to a power-of-two boundary if possible (BIOS usually does this).
- Use PAT to refine if needed.
- If you want the control block to be WC (write-combining) or WT
  (write-through), you can override via PAT on page tables.
- But for code execution, UC is mandatory.

**What not to do:**

- Do not mark the BAR WB (write-back).
- Do not rely on CLFLUSH or WBINVD to "fix" stale lines — it's fragile and
  racy with an external writer.

## 🧠 CPU Instruction Fetch Behaviour You Must Account For

- The CPU prefetches aggressively.
- Prefetch may cross function boundaries, region boundaries, and even into
  your control block if adjacent.
- Prefetch does not respect your logical "this is code / this is data"
  separation.

**Mitigation:**

- Align code regions to 4 KiB or 64 KiB boundaries.
- Place a UD2-filled guard page after each region.
- Keep the control block far away from executable regions.

## 🔄 ESP32 ↔ Phenom II Synchronisation Protocol

This is the part that determines whether your system is deterministic or
haunted.

**Requirements:**

- ESP32 writes code asynchronously into FPGA DDR3.
- CPU must not execute until the write is complete.
- CPU must not see partially updated descriptors.
- CPU must not execute stale cached instructions (solved by UC mapping).

### Correct solution: A versioned descriptor with double buffering

**Memory layout:**

```c
struct ControlBlock {
    uint32_t seq;        // incremented LAST
    uint32_t code_addr;  // offset inside BAR
    uint32_t code_len;
    uint32_t flags;      // e.g., "valid", "halt", etc.
    uint32_t checksum;   // optional
};
```

**ESP32 write sequence:**

1. Write code into Region B.
2. Compute checksum (optional).
3. Write descriptor fields except `seq`.
4. Issue ESP32 memory barrier.
5. Write `seq = seq + 1` (atomic from CPU's perspective).

**CPU polling loop:**

```c
uint32_t last_seq = 0;

while (1) {
    uint32_t s1 = ctrl->seq;
    barrier();
    read descriptor fields;
    barrier();
    uint32_t s2 = ctrl->seq;

    if (s1 == s2 && s1 != last_seq) {
        last_seq = s1;
        jump to code_addr;
    }
}
```

**Why this works:**

- If ESP32 is mid-update, `seq` changes between `s1` and `s2` → CPU ignores.
- If descriptor is stable, `seq` matches → safe to execute.
- No cache coherency issues because region is UC.

## ⚡ PCI BAR Execution Performance Reality

- **Theoretical PCI 32/33 bandwidth:** 133 MB/s.
- **Realistic sustained CPU read bandwidth:** 60–100 MB/s for long bursts.
- **Instruction fetch pattern:** Not long bursts — small, latency-bound reads.
- **Effective throughput for code execution:** Likely 10–40 MB/s equivalent,
  depending on instruction density and branch behaviour.

**Is that enough?** Yes. Even 10 MB/s is millions of instructions per second.

## 🧱 Additional Hard Pitfalls

### 1. FPGA must respond fast enough

PCI read latency must be low enough that the CPU doesn't stall excessively.
You may need:

- Read prefetch buffers
- Burst read support
- Deterministic response timing

### 2. BAR must be non-prefetchable

Set the PCI BAR flags to non-prefetchable. Otherwise the CPU/chipset may
issue speculative reads that violate your protocol.

### 3. ESP32 must not write into the region currently executing

Double buffering is mandatory.

### 4. FPGA DDR3 bandwidth (~133 MB/s)

This is your real bottleneck, not PCI.

## 🧭 A Minimal Working Example (MWEX) Boot Flow

1. BIOS hands control to your kernel.
2. Kernel enumerates PCI, finds FPGA BAR.
3. Kernel sets MTRR for BAR → UC.
4. Kernel maps control block + code regions.
5. Kernel enters polling loop.
6. ESP32 writes code into Region B.
7. ESP32 updates descriptor (`seq++`).
8. CPU sees new `seq`, jumps to Region B.
9. Repeat.

This is deterministic, safe, and avoids undefined behaviour.

## If you want next:

I can produce a full reference implementation of:

- Page table setup for UC BAR mapping
- MTRR programming code (AMD K10)
- The polling loop in x86-64 assembly
- The ESP32 side (C or Rust)
- A recommended BAR layout with alignment constraints
- A debug harness to detect stale instruction fetches

Just tell me whether you want the memory setup code, the synchronisation
protocol code, or the FPGA BAR layout design next.
