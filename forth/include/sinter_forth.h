/*
 * sinter_forth.h — subroutine-threaded Forth to x86-64 compiler (SINT-25).
 *
 * Plain, dependency-free C11. The compiler turns a small, deliberately
 * restricted Forth dialect into position-independent x86-64 machine code
 * written into a caller-supplied buffer. Nothing here allocates memory:
 * all compiler state lives in a caller-owned forth_ctx, so the same code
 * can later run on the ESP32 with the context in static storage.
 *
 * See forth/README.md for the dialect, and docs/notes/forth-compiler.md
 * for the design rationale.
 *
 * ------------------------------------------------------------------------
 * Emitted image layout (offsets from the start of the output buffer)
 * ------------------------------------------------------------------------
 *
 *   0x0000            jmp main                 (entry point, 5 bytes)
 *   0x0005            primitive library         (fixed blob, identical for
 *                                                every program; one `ret`-
 *                                                terminated routine per word)
 *   main_offset       main prologue
 *                     top-level code, with each colon definition compiled
 *                     in place and jumped over
 *                     main epilogue (`ret`)
 *   code_len - 16     UD2 guard tail (8 x 0F 0B)
 *
 * Every branch and call in the image is rel32, so the image is fully
 * position-independent: it can be copied to BAR + 0x1000 (or anywhere
 * else) and executed in place.
 *
 * ------------------------------------------------------------------------
 * Calling convention
 * ------------------------------------------------------------------------
 *
 * The entry point (offset 0) has the System V AMD64 signature
 *
 *     void entry(uint8_t *region);
 *
 * i.e. the caller passes the address of the scratch/result region in rdi.
 * The region is FORTH_REGION_SIZE bytes; in the rig it is BAR + 0x2000.
 *
 * Inside the image the Forth machine owns these registers:
 *
 *     rbx  top of data stack (cached)
 *     rbp  data-stack pointer: [rbp] is the second item; grows down
 *     r12  return-stack pointer (for >r r> r@ and do-loop indices);
 *          grows down. The hardware stack (rsp) remains the caller's and
 *          is used only for call/ret.
 *     r13  base address of the region (never changes)
 *     rax, rcx, rdx  scratch, clobbered freely by primitives
 *
 * rbx, rbp, r12 and r13 are callee-saved under System V, so the main
 * prologue pushes them and the epilogue restores them; the caller (the
 * kernel poll loop, or the test harness) never notices. The image uses
 * 32 bytes of the caller's stack plus one return address per
 * level of call nesting. rsp alignment is not maintained because the
 * image never calls out to foreign code.
 *
 * The prologue clears the done flag and error flags; the epilogue records
 * the final data-stack depth and top of stack so the ESP32 can inspect
 * them post mortem with a single SPI read.
 */
#ifndef SINTER_FORTH_H
#define SINTER_FORTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Scratch/result region layout (offsets from r13) ------------------
 *
 * These values are baked into the hand-authored machine code in words.c
 * and into the prologue/epilogue in emit.c. Changing one means changing
 * the bytes too; the execution tests will catch a mismatch.
 */
#define FORTH_REGION_SIZE   0x1000u /* whole region, one page            */
#define FORTH_REG_RESULT    0x00u   /* cell written by result!           */
#define FORTH_REG_DONE      0x08u   /* 1 once result! has run            */
#define FORTH_REG_DEPTH     0x10u   /* data-stack depth at exit, cells   */
#define FORTH_REG_TOS       0x18u   /* top of stack at exit (raw rbx)    */
#define FORTH_REG_FLAGS     0x20u   /* runtime error flags, see below    */
#define FORTH_USER_BASE     0x40u   /* first byte of program memory      */
#define FORTH_USER_END      0x800u  /* one past last byte of program mem */
#define FORTH_RSTACK_TOP    0xC00u  /* return stack: 0x800..0xBFF        */
#define FORTH_DSTACK_TOP    0x1000u /* data stack:   0xC00..0xFFF        */

/* Bits in the FORTH_REG_FLAGS cell. */
#define FORTH_FLAG_DIV_ZERO 0x1u    /* `/` or `mod` by zero was attempted */

/* @ and ! mask their address with this (cell-aligned, inside the region);
 * c@ and c! mask with FORTH_REGION_SIZE - 1. Generated code therefore
 * cannot touch memory outside the region whatever the program does. */
#define FORTH_CELL_ADDR_MASK 0xFF8u

/* Bytes of UD2 (0F 0B) appended after the program as a prefetch guard. */
#define FORTH_GUARD_BYTES   16u

/* ---- Compiler limits --------------------------------------------------- */
#define FORTH_MAX_NAME      31      /* longest colon-definition name      */
#define FORTH_MAX_DEFS      128     /* colon definitions per program      */
#define FORTH_MAX_CONTROL   32      /* nesting depth of control structures */
#define FORTH_MAX_MESSAGE   160     /* formatted error message, incl. NUL */

/* ---- Status codes ------------------------------------------------------ */
typedef enum forth_status {
    FORTH_OK = 0,
    FORTH_ERR_ARGUMENT,          /* NULL pointer or similar API misuse      */
    FORTH_ERR_UNKNOWN_WORD,      /* not a word in the dictionary or number  */
    FORTH_ERR_NUMBER_RANGE,      /* literal does not fit in a 64-bit cell   */
    FORTH_ERR_CONTROL_MISMATCH,  /* e.g. `then` without `if`, `loop` w/o do */
    FORTH_ERR_CONTROL_UNCLOSED,  /* `if`/`begin`/`do` still open at `;`/EOF */
    FORTH_ERR_CONTROL_DEPTH,     /* nesting deeper than FORTH_MAX_CONTROL   */
    FORTH_ERR_NESTED_DEFINITION, /* `:` inside a definition or control flow */
    FORTH_ERR_STRAY_SEMICOLON,   /* `;` outside a definition                */
    FORTH_ERR_UNTERMINATED_DEF,  /* end of input inside a definition        */
    FORTH_ERR_MISSING_NAME,      /* `:` at end of input                     */
    FORTH_ERR_BAD_NAME,          /* name too long or reserved               */
    FORTH_ERR_TOO_MANY_DEFS,     /* more than FORTH_MAX_DEFS definitions    */
    FORTH_ERR_UNTERMINATED_COMMENT, /* `(` without a closing `)`            */
    FORTH_ERR_OUTPUT_FULL        /* caller's buffer is too small            */
} forth_status;

/* Where and why compilation failed. `message` is a complete, human- and
 * LLM-readable sentence suitable for feeding straight back into the
 * agentic retry loop, e.g.
 *   "line 3, column 9: unknown word 'squared'" */
typedef struct forth_error {
    forth_status status;
    unsigned line;               /* 1-based; 0 if not tied to a token */
    unsigned column;             /* 1-based */
    char message[FORTH_MAX_MESSAGE];
} forth_error;

/* A colon definition, as recorded by the compiler. Offsets are into the
 * output image; `end` is one past the definition's final `ret`. */
typedef struct forth_def {
    char name[FORTH_MAX_NAME + 1];
    uint32_t start;
    uint32_t end;
} forth_def;

/* An open control structure on the compile-time control stack. */
typedef struct forth_control {
    int kind;                    /* internal FORTH_CF_* value */
    uint32_t pos;                /* branch target, or rel32 field to patch */
    unsigned line, column;       /* where the opening word appeared */
} forth_control;

/* Compiler state. Allocate one (statically, on the heap or on a roomy
 * stack — it is about 6 KiB) and pass it to forth_compile(). Fields below
 * the "results" block are private to the compiler. */
typedef struct forth_ctx {
    /* ---- results, valid after forth_compile() ---- */
    forth_error error;           /* status FORTH_OK on success            */
    size_t code_len;             /* bytes of image written, incl. guard   */
    size_t main_offset;          /* offset of the main prologue           */
    size_t ndefs;
    forth_def defs[FORTH_MAX_DEFS];

    /* ---- private ---- */
    const char *src;
    size_t src_len;
    size_t pos;                  /* tokeniser position in src           */
    unsigned line, column;       /* position of the next character      */
    const char *tok;             /* current token (not NUL-terminated)  */
    size_t tok_len;
    unsigned tok_line, tok_column;
    uint8_t *out;
    size_t cap;
    size_t len;                  /* bytes emitted so far                */
    int in_def;                  /* inside a colon definition           */
    size_t def_skip;             /* rel32 of the jmp over the open def  */
    forth_def pending;           /* definition being compiled           */
    size_t ncontrol;
    forth_control control[FORTH_MAX_CONTROL];
} forth_ctx;

/* Compile `src` (src_len bytes, need not be NUL-terminated) into `buf`.
 * On success returns FORTH_OK and sets ctx->code_len; the image entry
 * point is offset 0. On failure returns the error status and fills in
 * ctx->error; the buffer contents are then unspecified. */
forth_status forth_compile(forth_ctx *ctx, const char *src, size_t src_len,
                           uint8_t *buf, size_t buf_cap);

/* Short identifier for a status code, e.g. "unknown-word". */
const char *forth_status_name(forth_status status);

/* Look up a colon definition compiled into the last image by name
 * (case-insensitive; the newest definition wins). NULL if absent. */
const forth_def *forth_find_def(const forth_ctx *ctx, const char *name);

/* Size in bytes of the fixed primitive library, and the image offset at
 * which a named primitive's code starts. Return 0 / -1 respectively if
 * `name` is not a library primitive (control words have no library code). */
size_t forth_library_size(void);
long forth_primitive_offset(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SINTER_FORTH_H */
