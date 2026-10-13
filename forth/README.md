# Sinter Forth compiler (SINT-25, slice 1)

A subroutine-threaded compiler from a small Forth dialect to x86-64
machine code, written as a plain C11 library with no dependencies beyond
libc. It will eventually run on the ESP32 and produce the code that gets
injected into the Phenom II through the FPGA's PCI BAR; for now it builds
and is tested on the host (Linux or macOS). The design rationale is in
[`docs/notes/forth-compiler.md`](../docs/notes/forth-compiler.md), and the
execution environment the output targets is described in
[`docs/notes/pci-bar-execute.md`](../docs/notes/pci-bar-execute.md).

<!-- toc -->

- [Building and testing](#building-and-testing)
- [The dialect](#the-dialect)
- [Semantics worth knowing](#semantics-worth-knowing)
- [Image layout](#image-layout)
- [Calling convention](#calling-convention)
- [The scratch/result region](#the-scratchresult-region)
- [Compile errors](#compile-errors)
- [Adding a word](#adding-a-word)
- [Tests](#tests)
- [Deliberately deferred](#deliberately-deferred)

<!-- tocstop -->

## Building and testing

```bash
cd forth
make test          # builds with -Wall -Wextra -Werror and runs every suite
make CC=clang test # same with clang
make golden        # regenerate tests/golden/*.golden (see "Tests")
```

`make` builds three things under `build/`:

| Output | What it is |
|---|---|
| `libsinterforth.a` | the compiler (`src/`); the only part destined for the ESP32 |
| `forthc` | command-line front end: `forthc [-o FILE] [-x] [-r] [SOURCE]` |
| `forth_tests` | the test runner |

`forthc -r prog.fs` compiles and runs a program on an x86-64 host and
prints the result region; `forthc -o prog.bin prog.fs` writes the raw
image, which you can inspect with
`objdump -D -b binary -m i386:x86-64 -M intel prog.bin`.

Using the library from C:

```c
#include "sinter_forth.h"

static forth_ctx ctx;              /* ~6 KiB; no heap allocation anywhere */
static uint8_t image[16384];

if (forth_compile(&ctx, src, src_len, image, sizeof image) != FORTH_OK) {
    send_to_llm(ctx.error.message);   /* "line 3, column 9: unknown word 'sqaure'" */
} else {
    copy_to_bar(image, ctx.code_len); /* entry point is offset 0 */
}
```

## The dialect

This is the starting subset from §4 of the design note, and nothing more.
The LLM is told to stay inside it; anything else is a compile error that
goes back to the LLM as feedback. All values are 64-bit signed cells.

| Group | Words | Stack effect |
|---|---|---|
| Stack | `dup` `drop` `swap` `over` `rot` | `( x -- x x )` `( x -- )` `( a b -- b a )` `( a b -- a b a )` `( a b c -- b c a )` |
| Return stack | `>r` `r>` `r@` | `( x -- ) (R: -- x)` `( -- x ) (R: x -- )` `( -- x ) (R: x -- x)` |
| Arithmetic | `+` `-` `*` `/` `mod` | `( a b -- c )` |
| | `negate` `abs` `1+` `1-` | `( n -- n' )` |
| | `min` `max` | `( a b -- c )` |
| Logic | `and` `or` `xor` | `( a b -- c )` bitwise |
| | `invert` | `( n -- ~n )` |
| Comparison | `=` `<>` `<` `>` | `( a b -- flag )` signed |
| | `0=` `0<` | `( n -- flag )` |
| Memory | `@` `!` | `( addr -- x )` `( x addr -- )` 64-bit |
| | `c@` `c!` | `( addr -- byte )` `( byte addr -- )` |
| Control | `if` `else` `then` | `if ( flag -- )` |
| | `begin` `until` / `begin` `while` `repeat` | `until ( flag -- )` `while ( flag -- )` |
| | `do` `loop` `i` | `do ( limit start -- )` `i ( -- index )` |
| Definitions | `:` *name* … `;` | |
| Result | `result!` | `( x -- )` store the program's answer |
| Literals | `42` `-7` `$ff` `0xFF` `-$10` | `( -- n )` |

Comments are `( … )` and `\` to end of line. Words are case-insensitive.

Code outside colon definitions is the program's main body and runs top
to bottom. Each word compiles to a `call` into the primitive library (or
to an earlier colon definition); literals are pushed in line; control
words compile to `jz`/`jne`/`jmp` with rel32 displacements, patched in a
single pass through a small control-flow stack.

## Semantics worth knowing

- **Flags**: true is -1 (all bits set), false is 0. Anything non-zero
  counts as true for `if`, `until` and `while`.
- **Division** truncates towards zero (symmetric, like C), and `mod` takes
  the sign of the dividend. Neither can fault: dividing by zero gives
  `0` for `/` and the dividend for `mod` and sets the division-by-zero
  flag in the region; dividing by -1 is done without `idiv` so
  `INT64_MIN -1 /` wraps instead of raising #DE. Overflow elsewhere wraps.
- **Addresses** given to `@ ! c@ c!` are offsets into the scratch/result
  region, masked so the program can never touch memory outside it: `@`
  and `!` use `addr & 0xFF8` (so cell accesses are also rounded down to
  8-byte alignment), `c@` and `c!` use `addr & 0xFFF`. Programs should
  keep their own data in 0x40–0x7FF; see the region layout below.
- **`do … loop`** runs from `start` up to `limit - 1` and always runs the
  body at least once, as in standard Forth, so `0 0 do` runs 2⁶⁴ times.
  `i` is the innermost loop index. There is no `j`, `leave`, `?do` or
  `+loop` yet; keep an outer index in memory if you need one.
- **Definitions** become visible after their `;`, so there is no
  recursion yet. Redefining a word shadows the old meaning for code
  compiled afterwards; control words, `:` and `;` cannot be redefined.
- **`result!`** stores the value, sets the done flag and reads the flag
  back (the posted-write readback fence from §4 of the PCI note). It can
  be called more than once; the last value wins.
- **No runtime checking** of stack underflow or overflow. The epilogue
  records the final depth, so an unbalanced program shows up as a
  non-zero (or negative) depth in the region afterwards.

## Image layout

```
0x0000          jmp main                 entry point (5 bytes)
0x0005          primitive library        fixed blob, the same for every program
main_offset     prologue
                top-level code, with each colon definition compiled
                in place and jumped over
                epilogue, ret
code_len - 16   8 × UD2                  guard against running off the end
```

The library is emitted in word-table order, so its contents and every
primitive's offset are identical for every program. All calls and
branches are rel32, so the image is position-independent and can run
from `BAR + 0x1000` or anywhere else. The UD2 tail is the prefetch guard
from §2 of the PCI note.

## Calling convention

The entry point has the System V AMD64 signature

```c
void entry(uint8_t *region);   /* rdi = scratch/result region (BAR + 0x2000 on the rig) */
```

| Register | Use inside the image |
|---|---|
| `rbx` | top of the data stack (cached) |
| `rbp` | data-stack pointer; `[rbp]` is the second item; grows down |
| `r12` | return-stack pointer for `>r r> r@` and loop frames; grows down |
| `r13` | base address of the region |
| `rax` `rcx` `rdx` | scratch |
| `rsp` | the caller's stack, used only for `call`/`ret` |

`rbx`, `rbp`, `r12` and `r13` are callee-saved, so the prologue pushes
them and the epilogue restores them; the kernel poll loop never notices.
The image uses 32 bytes of the caller's stack for those registers, plus one
return address per level of call nesting.

## The scratch/result region

The region is one 4 KiB page, and both Forth stacks live inside it so the
ESP32 can read them back afterwards for post-mortem debugging.

| Offset | Contents |
|---|---|
| `0x000` | result (written by `result!`) |
| `0x008` | done flag (cleared by the prologue, set to 1 by `result!`) |
| `0x010` | data-stack depth at exit, in cells (written by the epilogue) |
| `0x018` | top of stack at exit (written by the epilogue) |
| `0x020` | error flags (cleared by the prologue; bit 0 = division by zero) |
| `0x028`–`0x03F` | reserved |
| `0x040`–`0x7FF` | program memory |
| `0x800`–`0xBFF` | return stack (top at `0xC00`) |
| `0xC00`–`0xFFF` | data stack (top at `0x1000`) |

The constants are in `include/sinter_forth.h` (`FORTH_REG_*` and friends).
They are also baked into the hand-written machine code, so changing one
means changing the bytes too (the execution tests will catch a mismatch).

## Compile errors

`forth_compile()` stops at the first error and fills in `ctx.error` with
a status code, a 1-based line and column, and a complete sentence meant
to go straight back to the LLM, for example:

```
line 1, column 5: unknown word 'frob'
line 1, column 5: 'then' without a matching 'if'
line 1, column 12: 'if' at line 1, column 7 is not closed before ';'
line 1, column 10: definition of 'f' is missing its ';'
line 1, column 1: number '9223372036854775808' does not fit in a 64-bit cell
```

The full list of statuses is the `forth_status` enum in the header:
unknown words, out-of-range numbers, mismatched, unclosed or too deeply
nested control flow, nested or unterminated definitions, stray `;`,
missing, over-long or reserved names, too many definitions, unterminated
comments and an output buffer that is too small.

## Adding a word

Everything the compiler knows is the table at the bottom of
[`src/words.c`](src/words.c).

**A primitive** (a word that is just "run this machine code"):

1. Write the instructions in Intel syntax, assemble them with GNU `as`,
   and copy the bytes into a `static const uint8_t p_name[]` array in
   `src/words.c`, with each instruction in a comment alongside. Follow the
   register convention above, end with `ret`, and do not use locked
   instructions (including `xchg` with memory): on the rig the stacks are
   in the PCI BAR, where a locked access becomes a bus lock.
2. Add one entry to the table, e.g. `PRIM("2*", p_two_star),`. Append new
   entries at the end of their group or at the end of the table, as
   inserting one shifts the offsets of everything after it (harmless,
   but it changes every golden image).
3. Add its bytes and expected `objdump -M intel` disassembly to
   `prim_goldens[]` in `tests/test_golden.c` (the suite fails if a
   primitive has no golden entry), add a behavioural test to
   `tests/test_exec.c`, and run `make golden` to refresh the whole-image
   goldens.

**A compile-time word** (control flow, or anything that needs to emit
code or patch branches): write a `forth_status c_name(forth_ctx *, const
forth_word *)` function using the emitters in `src/internal.h`
(`forth_emit_call`, `forth_emit_0branch`, `forth_emit_jmp`,
`forth_emit_rel32`, `forth_patch`) and the control-flow stack, then add
`COMPILE("name", c_name),`. Helpers that only compile-time words should
reach go in the library with `HIDDEN("(name)", p_name)`; `do` and `loop`
work this way.

## Tests

`make test` runs four suites (about 15,000 checks, a few seconds):

- **golden**: byte-exact checks of every primitive, the call emitted for
  each word, literals of each encoding width, every control-flow shape
  (`if`/`then`, `if`/`else`/`then`, `begin`/`until`,
  `begin`/`while`/`repeat`, `do`/`loop`, and nested combinations), and
  whole images against `tests/golden/*.golden`. After an intended change
  to the output, run `make golden` and review the diff (and the
  disassembly) before committing.
- **errors**: every error status, with its line, column and message, and
  every possible too-small output buffer.
- **disasm**: runs GNU objdump over each primitive and checks the exact
  instruction sequence, then over whole images to check that they decode
  cleanly with instruction boundaries at the entry, every primitive, main
  and every definition, and only UD2 in the tail. Skipped if GNU objdump
  is missing (macOS ships LLVM's; `brew install binutils` and
  `make test OBJDUMP=gobjdump` to run it there).
- **exec**: compiles programs and runs them through `host/exec.c`, which
  maps the image as executable (writable first, then read+execute, never
  both), passes an ordinary buffer as the region, and catches faults and
  runaway loops. It checks every arithmetic, logic and comparison word
  against a C reference over a grid of values including `INT64_MIN` and
  `INT64_MAX`, the stack words, nested definitions, every loop form,
  memory reads and writes against data planted by the "ESP32", a prime
  sieve, a fixed-point Mandelbrot smoke test, the region protocol,
  position independence (the same image run from an odd address) and the
  UD2 guard. Runs on x86-64 Linux and Intel macOS; skipped elsewhere.

## Deliberately deferred

Not in this slice, by design:

- **ESP-IDF integration**: the library is ready to drop into a component
  (no heap, no I/O, context in caller-owned storage), but nothing under
  `embedded/esp32/` uses it yet.
- **The PCI BAR write path**: copying the image to `BAR + 0x1000`, the
  status-word handshake and the kernel-side caller (SINT-95). The host
  harness stands in for all of it.
- **Floating point, strings and output words** (`."`, `emit` and so on).
  Results come back through the region, and Mandelbrot works in fixed
  point.
- **More words**: `j`, `leave`, `?do`, `+loop`, `exit`, `recurse`,
  shifts, double-cell arithmetic, `variable`/`constant`/`create`. Each
  should arrive when a workload needs it (§1 and §7 of the design note).
- **Runtime stack checking** and **inlining** of primitives: the design
  note's "native inlining" upgrade path. The table already separates the
  library code from how a word is compiled, so a per-word "inline" flag is
  a small change when it is wanted.
- **Tail-call and short-branch optimisation**: every call and branch is
  rel32 for now.
