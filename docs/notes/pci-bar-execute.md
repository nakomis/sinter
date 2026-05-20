# Executing code directly from an FPGA PCI BAR (SINT design notes)

Working notes for the Sinter execution path:

```
ESP32 → SPI → FPGA → FPGA DDR3 → PCI BAR → Phenom II fetches & executes
```

The eventual goal is for the Phenom II X4 965 to fetch x86-64 instructions
directly out of the Tang Primer 20K FPGA's DDR3, mapped as a PCI memory-mapped
BAR in one of the GA-M68MT-S2's two legacy 32-bit / 33 MHz PCI slots — with
the ESP32 asynchronously updating the code in DDR3 while the Phenom polls a
status word. This file is the running list of gotchas to design for; nothing
here has been verified on the real rig yet (the BIOS dump in SINT-34 is the
current blocker).

## 1. Cache coherency is *the* gotcha — mark the BAR Uncacheable

This is the single most important thing on the page.

By default the Phenom II treats the BAR's physical address range like any
other memory: it caches reads in L1/L2/L3 and may buffer writes. The first
time the CPU fetches an instruction from the BAR, the cache line goes into
L1i. If the ESP32 then rewrites the FPGA DDR3 underneath, the CPU will
**not see the change** — there's no MESI traffic from a PCI device into
the CPU caches, so the cache lines stay valid from the CPU's point of view
and it happily keeps executing the stale code.

The fix is to mark the BAR's physical region **Uncacheable (UC)** so every
instruction fetch goes out over HyperTransport → MCP68 → PCI to the FPGA.
Two mechanisms, either is fine:

- **MTRRs** (Memory Type Range Registers) — set a variable-range MTRR
  covering the BAR with type `UC` (0x00). Cheap, set once at kernel init,
  applies physically.
- **PAT / page-table attributes** — flag the pages mapping the BAR with
  the PAT/PCD/PWT bits set to UC. Finer-grained but requires paging to be
  already up.

Either makes the CPU bypass its caches for the BAR; both is belt-and-braces.
`WC` (Write-Combining) is tempting for streaming writes back to the FPGA,
but for *instruction fetch* you want plain UC — anything else risks the
CPU coalescing or reordering reads in ways that interact with prefetch.

`UC-` (UC minus) exists for cases where you want MTRR to say WB but PAT to
override to UC; not relevant here unless a downstream firmware change
forces a WB MTRR over the region.

### Test for the bug

If the kernel ever appears to be ignoring ESP32 updates, the diagnostic is:
issue a `wbinvd` (or specific `clflush` lines) and retry. If the new code
suddenly starts running, the BAR isn't UC.

## 2. Instruction prefetch past the BAR boundary

Even with UC, the Phenom II's front-end may speculatively prefetch ahead
of the current `RIP`. On a UC region this prefetch is bounded (the CPU
isn't allowed to *cache* speculative UC reads) but it can still issue
extra PCI transactions for instructions past a branch.

Implications:

- Don't put MMIO-side-effect registers in the same MMIO line(s) as
  executable code. The ESP32-writable status word should be in a
  *separate* page (or at least separate cache-line-sized region) from the
  injected code.
- A safe layout for the BAR is:
  - `BAR + 0x000`: control / status registers (UC).
  - `BAR + 0x1000`: code region (UC, page-aligned).
  - `BAR + 0x2000`: scratch / result region (UC or WC depending on use).

## 3. Status-register handshake

The ESP32 writes asynchronously; the Phenom must not jump to half-written
code. Idiomatic PCI co-processor handshake:

| Offset      | Meaning                                                |
|-------------|--------------------------------------------------------|
| `BAR + 0x0` | Status word (Phenom polls). `0` = busy, `1` = ready.   |
| `BAR + 0x4` | Length of valid code, bytes.                           |
| `BAR + 0x8` | Reserved (version / magic for sanity).                 |
| `BAR + 0x1000` | Code entry point (page-aligned, see §2).            |

Workflow:

1. ESP32 writes status = `0` (busy).
2. ESP32 streams code bytes into `BAR + 0x1000…`.
3. ESP32 writes the length to `BAR + 0x4`.
4. ESP32 writes status = `1` (ready) — *last*.
5. Phenom polls status word; on seeing `1`, jumps to `BAR + 0x1000`.
6. Injected code ends with `ret` so control returns to the kernel poll loop.

The "status last" ordering matters: any other write order can have the
Phenom jump while the code is still being filled. The FPGA's PCI target
core should guarantee write ordering within a single requester (which the
ESP32 effectively is, behind the FPGA).

## 4. Executing in-place vs. copy-then-jump

The poetry of the project is "Phenom executes code served directly by the
FPGA". The deterministic alternative is "Phenom `memcpy`s the code into
system RAM, `wbinvd`s, then jumps".

| | In-place (`call (BAR + 0x1000)`) | Copy-then-jump |
|---|---|---|
| Cache risk | None if UC | None (WB system RAM, flushed before exec) |
| Speed | Limited to ~80–110 MB/s sustained PCI burst | First fetch slow, then full L1 speed (~tens of GB/s) |
| Determinism | Subject to prefetch quirks (see §2) | Standard |
| Project ethos | ✅ matches "as directly as possible" | ❌ feels like cheating |

For early bring-up, copy-then-jump is the right tool — it isolates "does the
PCI link work" from "does in-place execution work". Once that's solid,
move to in-place and keep copy-then-jump as the fallback path.

## 5. Realistic PCI bandwidth

Legacy 32-bit / 33 MHz PCI peaks at **133 MB/s theoretical**. In practice
~80–110 MB/s sustained burst is typical, less for small individual reads
(address phase + turnaround overhead).

This is plenty for the workloads in the README — pi digits, Mandelbrot,
prime sieves are compute-bound on the Phenom II, not link-bound. A
~1 MB binary loaded once and executed for minutes is fine. If a workload
ever wants to stream tens of MB/s of *code* from the FPGA, the project
will have outgrown PCI and want either PCIe (different FPGA) or a
copy-then-jump model with system RAM as the working set.

## 6. PCI device discovery

The FPGA's PCI target core must respond to configuration-space reads with
a real Vendor/Device ID. Two options:

- **Allocate a real Vendor ID** — needs PCI-SIG membership; not happening.
- **Use a private/locally-administered ID** — pick something obviously
  ours (e.g. `0xDEAD:0xBEEF`), document it in the FPGA source. Fine for
  hobby use; not standards-compliant but no one will know.

The kernel's PCI enumerator finds it by scanning bus 0 (and any
chipset-presented buses) for the chosen IDs. **Don't** code against
MCP68 device IDs sourced from any LLM without first running `lspci -nn`
on the actual board from a Linux live USB and saving the output to this
repo — the IDs hallucinate freely and rev 1.3 vs 3.1 may differ.

## 7. Order of bring-up (suggested)

1. Tang Primer 20K + ESP32 over SPI, no AM3 board involved. ESP32 can
   read/write FPGA DDR3 end-to-end. *(SINT-FPGA-01-ish.)*
2. FPGA PCI target core present in a card-edge breakout, GA-M68MT-S2
   sees it under `lspci` — boot from a Linux live USB just to confirm.
3. Custom kernel (or a small stub inside an existing kernel) finds the
   device, reads the BAR size, writes the BAR address into config space,
   reads back a magic value from `BAR + 0x8`.
4. Mark BAR UC via MTRR, read/write status word from kernel.
5. ESP32 writes a known instruction sequence (e.g. `mov rax, 0x42; ret`),
   Phenom calls it as a function pointer, validates `rax`.
6. Full agentic loop: pseudocode → Bedrock → Forth → compile → SPI →
   FPGA → BAR → execute → SATA disk → validation.

Each step is independently testable; don't conflate them.

## 8. What this file is *not*

This is a design-notes file, not a spec. None of it has been verified on
silicon — the rig is still blocked on getting a verified BIOS dump
(SINT-34). Treat every claim as "best current understanding"; revise as
the actual hardware lies to us.
