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
- **Fill the tail of the code region with `UD2` (`0F 0B`) as a guard.**
  `UD2` is the architecturally-guaranteed invalid opcode — it raises `#UD`
  (invalid opcode exception) reliably. The BAR's hardware address decode
  prevents fetches *past* the BAR, but speculative prefetch *inside* the
  BAR past the end of the injected code is fair game. Padding with `UD2`
  turns "wandered off the end" from "executes random garbage" into "clean
  trap the kernel can catch and report".

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

### Alternative: double-buffered code regions (A/B)

Once the single-buffer handshake above is solid, the natural upgrade is
A/B double-buffering, which lets the ESP32 prepare the *next* code block
while the Phenom is still running the *previous* one:

| Offset             | Meaning                                                |
|--------------------|--------------------------------------------------------|
| `BAR + 0x00`       | Active region: `0` = A, `1` = B. Phenom polls this.    |
| `BAR + 0x04`       | A: sequence number (incremented when A is fresh).      |
| `BAR + 0x08`       | B: sequence number (incremented when B is fresh).      |
| `BAR + 0x1000`     | Code region A (UC, page-aligned).                      |
| `BAR + 0x2000`     | Code region B (UC, page-aligned).                      |

Workflow:

1. ESP32 picks the *inactive* region (the one Phenom isn't running).
2. ESP32 writes code into it, then increments that region's sequence
   number.
3. ESP32 flips the active-region word.
4. Phenom finishes whatever it's running, reads the active-region word,
   reads the corresponding sequence number, and only jumps if the
   sequence number is stable across two reads (catches mid-flux).

This eliminates the "Phenom stalls during code reload" problem and gives
you a built-in sanity check via the sequence numbers. Worth doing once
the basic XIP works, not before.

### CPU-side serialisation before the jump

Even with UC mapping and the status-last handshake, the Phenom's front-end
will have speculatively prefetched and predecoded instructions around the
polling loop. Before calling into the freshly-written code, drain the
pipeline explicitly:

```asm
    mfence              ; finish any pending loads/stores
    cpuid               ; cpuid is a serialising instruction — flushes
                        ;   the front-end and re-fetches downstream of
                        ;   this point
    call    [bar_code]  ; jump to BAR + 0x1000
```

`cpuid` is the canonical serialising instruction on x86 — it guarantees
that everything before it is complete and everything after it is freshly
fetched. `mfence` alone is *not* enough for instruction-stream serialisation
(it orders memory operations, not the front-end). Don't reach for `invd` —
it invalidates without write-back and is a footgun if anything else in the
kernel is using cacheable memory at the time. `wbinvd` (write-back +
invalidate) is the safe equivalent if you actually need to drop cache lines,
but on a properly UC-mapped BAR you don't.

## 4. PCI posted-write readback fences

NVIDIA nForce-era chipsets are infamously aggressive about buffering MMIO
writes. When the Phenom writes a value back to the FPGA — say, a "done"
status or a result word — the chipset may hold that write in an internal
buffer rather than letting it traverse the bus immediately. The ESP32
polling the FPGA from the other side will see *nothing* until something
forces the buffer to flush.

The standard idiom: **immediately read back the same MMIO address** after
the write. The read can't be satisfied from the write buffer, so it forces
the buffered write out first.

```c
*((volatile uint32_t *)(bar + STATUS)) = STATE_DONE;
(void)*((volatile uint32_t *)(bar + STATUS));   /* readback fence */
```

This matters most for the Phenom → FPGA direction (status updates, command
acknowledgements, result bytes). FPGA → Phenom writes from the ESP32 don't
have the same problem because they originate outside the chipset's posted-
write buffers.

## 5. Executing in-place vs. copy-then-jump

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

## 6. Realistic PCI bandwidth

Legacy 32-bit / 33 MHz PCI peaks at **133 MB/s theoretical**. In practice
**~50–90 MB/s sustained** for code-fetch-style reads from a BAR (small,
latency-sensitive, not very burst-friendly), rising to ~80–110 MB/s for
large bulk DMA reads where the chipset can burst freely. Individual reads
pay an address phase + turnaround cycle, so tight polling loops that read
one word at a time will land at the low end.

FPGA-side DDR3 throughput (~133 MB/s on the Tang Primer 20K's onboard
chip) is irrelevant to this number — PCI is the bottleneck. Expect
instruction fetch latency from the BAR to dwarf system RAM by an order of
magnitude or more.

This is plenty for the workloads in the README — pi digits, Mandelbrot,
prime sieves are compute-bound on the Phenom II, not link-bound. A
~1 MB binary loaded once and executed for minutes is fine. If a workload
ever wants to stream tens of MB/s of *code* from the FPGA, the project
will have outgrown PCI and want either PCIe (different FPGA) or a
copy-then-jump model with system RAM as the working set.

## 7. PCI device discovery

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

## 8. MCP68 / nForce-era quirks the kernel needs to know about

Captured from cross-referenced LLM assessments (ChatGPT, Grok) of common
nForce-era footguns. Treat as "things to look for", not "confirmed bugs":

- **ACPI tables are ugly.** Linux carries piles of nForce quirks in its ACPI
  parser. Don't trust MADT / HPET / SCI routing without booting Linux on the
  same board first and comparing to your own parser's output.
- **NVIDIA SATA quirks** — NCQ bugs, DMA timeout oddities. Bring storage up
  in **legacy IDE mode** (BIOS setting) and stick to PIO before adding
  DMA. AHCI on nForce is famously more painful than on Intel.
- **GeForce 7025 iGPU MMIO is undocumented.** If we ever drive the
  framebuffer directly rather than via VESA/VBE, expect reverse-engineering
  via the nouveau source. For now: VESA linear framebuffer is fine and free.
- **Power management / clocking** — C-state and P-state transitions while
  the kernel doesn't know how to handle them can cause apparently-spontaneous
  hangs. Mask them in MSRs early during bring-up; revisit when stable.
- **Interrupt routing** — APIC layout is mostly standard but with NVIDIA
  tweaks. Use the **8259 PIC** for the first milestones; move to IOAPIC /
  LAPIC / MSI only when the rest is solid.
- **Documentation is poor.** Datasheets aren't public for the MCP series.
  Authoritative sources are: Linux kernel sources, `pci.ids`,
  `lspci -vv` / config-space dumps from the real board.

## 9. Order of bring-up (suggested)

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

## 10. Cross-LLM consultation, 2026-05-20 (Cabal MCP first run)

Ran the design past the Cabal MCP (Bedrock Mistral / Llama / Nova, Azure
Grok 4.3, Gemini 3 Pro — gpt-5.4-pro pending Azure quota). Raw replies in
`docs/notes/research/20260520-171539Z-sinter-mcp-cabal-first-run-*.md`.

**Prompt-framing failure to flag (apply the "verify the question, not just
the answer" principle from `multi-llm-consultation.md`):** my Cabal prompt
asked about **PCIe x1 Gen1**. The actual design uses **legacy 32-bit /
33 MHz PCI** (see §6). The Tang Primer 20K *cannot* drive PCIe Gen1
(no SerDes, fabric can't hit 2.5 GT/s) — every model agreed on that — but
it *can* drive legacy PCI in soft logic, which is what the design assumes.
None of the five models caught that I was asking the wrong question, even
under the "be blunt, flag malformed questions" framing. So the "PCIe x1
infeasible" consensus is correct-but-irrelevant for this project.

Bits that **do** apply once you re-read the responses with PCI 32/33 in
mind:

- **Bootstrap dependency (Gemini, unique).** The x86 reset vector points
  to BIOS ROM, not the FPGA BAR. You need a BIOS + a host kernel (or
  custom firmware) running on the Phenom *before* anything can enumerate
  the PCI bus, assign the BAR, and jump to its contents. §9 already
  starts with "boot a Linux live USB", but make this explicit: there is
  no path where the FPGA owns first instruction fetch. The Phenom *must*
  boot something else first.

- **W^X discipline as the safe pattern (Gemini).** Real JITs and module
  loaders write code into a non-executable mapping, fence, flush, *then*
  flip permissions and jump. Our handshake (§3) does the equivalent with
  the status word — keep it that way; don't relax it.

- **No CPU snoop path for instruction fetch (Grok, single-sentence
  framing).** Reinforces §1's UC argument: PCI devices never participate
  in MESI, so the only way to keep the I-cache honest is to bypass it
  entirely. Already covered; Grok said it more pithily than this file
  does.

- **Cache-mode disagreement across models is real, not noise.** Mistral
  said WB, Llama said WT/UC, Nova said UC/WC, Grok said WB-or-WT, Gemini
  said UC. Only Grok and Gemini explained *why*, and they reach opposite
  conclusions (Grok: WB needed for atomic cache-line fills; Gemini: UC
  needed because no coherency). Our existing §1 picks UC and explains
  why; the cabal didn't shift that. But worth verifying against AMD's
  BKDG before treating it as settled — none of the five models is an
  authoritative source on K10-family cache semantics.

Cost of the consultation: $0.021 USD. Five voices, written to disk, in
~45 seconds. Pattern is repeatable — see `multi-llm-consultation.md` for
when to reach for it.

## 11. What this file is *not*

This is a design-notes file, not a spec. None of it has been verified on
silicon — the rig is still blocked on getting a verified BIOS dump
(SINT-34). Treat every claim as "best current understanding"; revise as
the actual hardware lies to us.
