/*
 * words.c — the word table: every word the dialect knows.
 *
 * Two kinds of entry:
 *
 *   - Primitives carry hand-authored x86-64 bytes (ending in `ret`). They
 *     are concatenated, in table order, into the primitive library at the
 *     start of every image; using the word compiles to `call <primitive>`.
 *
 *   - Compile-time words (control flow, `:` and `;`) carry a compile
 *     function instead, which emits branches and patches fixups.
 *
 * To add a primitive: write its bytes as a `static const uint8_t p_xxx[]`
 * below (with the assembly alongside, checked with objdump), and add one
 * table entry. Then add its golden bytes and disassembly to
 * tests/test_golden.c and a behavioural case to tests/test_exec.c.
 *
 * Register conventions are in include/sinter_forth.h: rbx = TOS, rbp =
 * data stack, r12 = return stack, r13 = region base; rax/rcx/rdx scratch.
 * Primitives must not touch any other register, and must not use locked
 * instructions (xchg with memory included): the stacks live in the PCI
 * BAR on the rig, where a locked access becomes a bus lock.
 */
#include <stddef.h>
#include <string.h>

#include "internal.h"

/* ---- Stack ------------------------------------------------------------- */

static const uint8_t p_dup[] = {            /* ( x -- x x ) */
    0x48, 0x83, 0xED, 0x08,                 /* sub  rbp, 8              */
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_drop[] = {           /* ( x -- ) */
    0x48, 0x8B, 0x5D, 0x00,                 /* mov  rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_swap[] = {           /* ( a b -- b a ) */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_over[] = {           /* ( a b -- a b a ) */
    0x48, 0x83, 0xED, 0x08,                 /* sub  rbp, 8              */
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */
    0x48, 0x8B, 0x5D, 0x08,                 /* mov  rbx, [rbp+8]        */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_rot[] = {            /* ( a b c -- b c a ) */
    0x48, 0x8B, 0x45, 0x08,                 /* mov  rax, [rbp+8]        */
    0x48, 0x8B, 0x4D, 0x00,                 /* mov  rcx, [rbp]          */
    0x48, 0x89, 0x4D, 0x08,                 /* mov  [rbp+8], rcx        */
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_to_r[] = {           /* ( x -- ) ( R: -- x ) */
    0x49, 0x83, 0xEC, 0x08,                 /* sub  r12, 8              */
    0x49, 0x89, 0x1C, 0x24,                 /* mov  [r12], rbx          */
    0x48, 0x8B, 0x5D, 0x00,                 /* mov  rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_r_from[] = {         /* ( -- x ) ( R: x -- ) */
    0x48, 0x83, 0xED, 0x08,                 /* sub  rbp, 8              */
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */
    0x49, 0x8B, 0x1C, 0x24,                 /* mov  rbx, [r12]          */
    0x49, 0x83, 0xC4, 0x08,                 /* add  r12, 8              */
    0xC3,                                   /* ret                      */
};
/* r@ and i are the same operation: copy the top of the return stack,
 * which inside a do-loop is the current index. Both get their own copy in
 * the library so that each table entry maps to exactly one routine. */
#define R_FETCH                                                            \
    0x48, 0x83, 0xED, 0x08,                 /* sub  rbp, 8              */ \
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */ \
    0x49, 0x8B, 0x1C, 0x24,                 /* mov  rbx, [r12]          */ \
    0xC3                                    /* ret                      */
static const uint8_t p_r_fetch[] = { R_FETCH };  /* ( -- x ) ( R: x -- x ) */
static const uint8_t p_i[] = { R_FETCH };        /* ( -- index )           */
#undef R_FETCH

/* ---- Arithmetic -------------------------------------------------------- */

static const uint8_t p_plus[] = {           /* ( a b -- a+b ) */
    0x48, 0x03, 0x5D, 0x00,                 /* add  rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_minus[] = {          /* ( a b -- a-b ) */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x48, 0x29, 0xD8,                       /* sub  rax, rbx            */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_star[] = {           /* ( a b -- a*b ) */
    0x48, 0x0F, 0xAF, 0x5D, 0x00,           /* imul rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
/* Division truncates towards zero (symmetric, like C and idiv). The two
 * inputs that would raise #DE — and take the Phenom's kernel down with
 * them — are handled without idiv: a zero divisor gives 0 and sets
 * FORTH_FLAG_DIV_ZERO; a divisor of -1 negates (so INT64_MIN / -1 wraps). */
static const uint8_t p_slash[] = {          /* ( a b -- a/b ) */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0x48, 0x85, 0xDB,                       /* test rbx, rbx            */
    0x74, 0x16,                             /* jz   .zero (+0x16)       */
    0x48, 0x83, 0xFB, 0xFF,                 /* cmp  rbx, -1             */
    0x74, 0x09,                             /* je   .neg (+0x09)        */
    0x48, 0x99,                             /* cqo                      */
    0x48, 0xF7, 0xFB,                       /* idiv rbx                 */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0xC3,                                   /* ret                      */
    0x48, 0xF7, 0xD8,                       /* .neg: neg rax            */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0xC3,                                   /* ret                      */
    0x49, 0x83, 0x4D, 0x20, 0x01,           /* .zero: or qword [r13+0x20], 1 */
    0x31, 0xDB,                             /* xor  ebx, ebx            */
    0xC3,                                   /* ret                      */
};
/* Remainder has the sign of the dividend. Zero divisor gives the dividend
 * and sets FORTH_FLAG_DIV_ZERO; divisor -1 gives 0. */
static const uint8_t p_mod[] = {            /* ( a b -- a mod b ) */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0x48, 0x85, 0xDB,                       /* test rbx, rbx            */
    0x74, 0x12,                             /* jz   .zero (+0x12)       */
    0x48, 0x83, 0xFB, 0xFF,                 /* cmp  rbx, -1             */
    0x74, 0x09,                             /* je   .m1 (+0x09)         */
    0x48, 0x99,                             /* cqo                      */
    0x48, 0xF7, 0xFB,                       /* idiv rbx                 */
    0x48, 0x89, 0xD3,                       /* mov  rbx, rdx            */
    0xC3,                                   /* ret                      */
    0x31, 0xDB,                             /* .m1: xor ebx, ebx        */
    0xC3,                                   /* ret                      */
    0x49, 0x83, 0x4D, 0x20, 0x01,           /* .zero: or qword [r13+0x20], 1 */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_negate[] = {         /* ( n -- -n ) */
    0x48, 0xF7, 0xDB,                       /* neg  rbx                 */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_abs[] = {            /* ( n -- |n| ) */
    0x48, 0x89, 0xD8,                       /* mov  rax, rbx            */
    0x48, 0xF7, 0xD8,                       /* neg  rax                 */
    0x48, 0x0F, 0x49, 0xD8,                 /* cmovns rbx, rax          */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_min[] = {            /* ( a b -- min ) */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0x48, 0x39, 0xD8,                       /* cmp  rax, rbx            */
    0x48, 0x0F, 0x4C, 0xD8,                 /* cmovl rbx, rax           */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_max[] = {            /* ( a b -- max ) */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0x48, 0x39, 0xD8,                       /* cmp  rax, rbx            */
    0x48, 0x0F, 0x4F, 0xD8,                 /* cmovg rbx, rax           */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_one_plus[] = {       /* ( n -- n+1 ) */
    0x48, 0x83, 0xC3, 0x01,                 /* add  rbx, 1              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_one_minus[] = {      /* ( n -- n-1 ) */
    0x48, 0x83, 0xEB, 0x01,                 /* sub  rbx, 1              */
    0xC3,                                   /* ret                      */
};

/* ---- Logic and comparison (true is -1, false is 0) ---------------------- */

static const uint8_t p_and[] = {
    0x48, 0x23, 0x5D, 0x00,                 /* and  rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_or[] = {
    0x48, 0x0B, 0x5D, 0x00,                 /* or   rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_xor[] = {
    0x48, 0x33, 0x5D, 0x00,                 /* xor  rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_invert[] = {
    0x48, 0xF7, 0xD3,                       /* not  rbx                 */
    0xC3,                                   /* ret                      */
};

/* ( a b -- flag ): compare a with b, signed. Only the setcc byte differs. */
#define COMPARE(setcc)                                                     \
    0x31, 0xC0,                             /* xor  eax, eax            */ \
    0x48, 0x39, 0x5D, 0x00,                 /* cmp  [rbp], rbx          */ \
    0x0F, setcc, 0xC0,                      /* setcc al                 */ \
    0x48, 0xF7, 0xD8,                       /* neg  rax                 */ \
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */ \
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */ \
    0xC3                                    /* ret                      */
static const uint8_t p_eq[] = { COMPARE(0x94) };   /* sete  */
static const uint8_t p_ne[] = { COMPARE(0x95) };   /* setne */
static const uint8_t p_lt[] = { COMPARE(0x9C) };   /* setl  */
static const uint8_t p_gt[] = { COMPARE(0x9F) };   /* setg  */
#undef COMPARE

static const uint8_t p_zero_eq[] = {        /* ( n -- flag ) */
    0x31, 0xC0,                             /* xor  eax, eax            */
    0x48, 0x85, 0xDB,                       /* test rbx, rbx            */
    0x0F, 0x94, 0xC0,                       /* sete al                  */
    0x48, 0xF7, 0xD8,                       /* neg  rax                 */
    0x48, 0x89, 0xC3,                       /* mov  rbx, rax            */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_zero_lt[] = {        /* ( n -- flag ) */
    0x48, 0xC1, 0xFB, 0x3F,                 /* sar  rbx, 63             */
    0xC3,                                   /* ret                      */
};

/* ---- Memory (addresses are offsets into the region, masked) ------------- */

static const uint8_t p_fetch[] = {          /* ( addr -- x ) */
    0x81, 0xE3, 0xF8, 0x0F, 0x00, 0x00,     /* and  ebx, 0xff8          */
    0x49, 0x8B, 0x5C, 0x1D, 0x00,           /* mov  rbx, [r13+rbx]      */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_store[] = {          /* ( x addr -- ) */
    0x81, 0xE3, 0xF8, 0x0F, 0x00, 0x00,     /* and  ebx, 0xff8          */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x49, 0x89, 0x44, 0x1D, 0x00,           /* mov  [r13+rbx], rax      */
    0x48, 0x8B, 0x5D, 0x08,                 /* mov  rbx, [rbp+8]        */
    0x48, 0x83, 0xC5, 0x10,                 /* add  rbp, 16             */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_c_fetch[] = {        /* ( addr -- byte ) */
    0x81, 0xE3, 0xFF, 0x0F, 0x00, 0x00,     /* and  ebx, 0xfff          */
    0x41, 0x0F, 0xB6, 0x5C, 0x1D, 0x00,     /* movzx ebx, byte [r13+rbx] */
    0xC3,                                   /* ret                      */
};
static const uint8_t p_c_store[] = {        /* ( byte addr -- ) */
    0x81, 0xE3, 0xFF, 0x0F, 0x00, 0x00,     /* and  ebx, 0xfff          */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x41, 0x88, 0x44, 0x1D, 0x00,           /* mov  [r13+rbx], al       */
    0x48, 0x8B, 0x5D, 0x08,                 /* mov  rbx, [rbp+8]        */
    0x48, 0x83, 0xC5, 0x10,                 /* add  rbp, 16             */
    0xC3,                                   /* ret                      */
};

/* ---- Result ------------------------------------------------------------- */

/* ( x -- ) Store x as the result and set the done flag, then read the
 * flag back: on the rig that read forces the chipset to flush the posted
 * writes (docs/notes/pci-bar-execute.md §4). */
static const uint8_t p_result_store[] = {
    0x49, 0x89, 0x5D, 0x00,                 /* mov  [r13], rbx          */
    0x49, 0xC7, 0x45, 0x08,
    0x01, 0x00, 0x00, 0x00,                 /* mov  qword [r13+8], 1    */
    0x49, 0x8B, 0x45, 0x08,                 /* mov  rax, [r13+8]        */
    0x48, 0x8B, 0x5D, 0x00,                 /* mov  rbx, [rbp]          */
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */
    0xC3,                                   /* ret                      */
};

/* ---- Hidden helpers for do ... loop -------------------------------------
 * Loop frame on the return stack: [r12] = index, [r12+8] = limit. */

static const uint8_t p_do[] = {             /* ( limit start -- ) */
    0x49, 0x83, 0xEC, 0x10,                 /* sub  r12, 16             */
    0x49, 0x89, 0x1C, 0x24,                 /* mov  [r12], rbx          */
    0x48, 0x8B, 0x45, 0x00,                 /* mov  rax, [rbp]          */
    0x49, 0x89, 0x44, 0x24, 0x08,           /* mov  [r12+8], rax        */
    0x48, 0x8B, 0x5D, 0x08,                 /* mov  rbx, [rbp+8]        */
    0x48, 0x83, 0xC5, 0x10,                 /* add  rbp, 16             */
    0xC3,                                   /* ret                      */
};
/* Increment the index and compare it with the limit. `ret` preserves the
 * flags, so the caller follows the call with `jne <loop body>`. */
static const uint8_t p_loop[] = {
    0x49, 0x8B, 0x04, 0x24,                 /* mov  rax, [r12]          */
    0x48, 0x83, 0xC0, 0x01,                 /* add  rax, 1              */
    0x49, 0x89, 0x04, 0x24,                 /* mov  [r12], rax          */
    0x49, 0x3B, 0x44, 0x24, 0x08,           /* cmp  rax, [r12+8]        */
    0xC3,                                   /* ret                      */
};

/* ---- Slice 2 (SINT-104) ------------------------------------------------- */

/* ( -- index ) of the next loop out: the loop frame below the innermost. */
static const uint8_t p_j[] = {
    0x48, 0x83, 0xED, 0x08,                 /* sub  rbp, 8              */
    0x48, 0x89, 0x5D, 0x00,                 /* mov  [rbp], rbx          */
    0x49, 0x8B, 0x5C, 0x24, 0x10,           /* mov  rbx, [r12+16]       */
    0xC3,                                   /* ret                      */
};

/* ( x u -- x' ): logical shift by u bits. x86 masks the count to 6 bits,
 * so counts of 64 or more (unsigned) are caught and give 0. */
#define SHIFT(modrm)                                                       \
    0x48, 0x89, 0xD9,                       /* mov  rcx, rbx            */ \
    0x48, 0x8B, 0x5D, 0x00,                 /* mov  rbx, [rbp]          */ \
    0x48, 0x83, 0xC5, 0x08,                 /* add  rbp, 8              */ \
    0x48, 0xD3, modrm,                      /* shl/shr rbx, cl          */ \
    0x31, 0xC0,                             /* xor  eax, eax            */ \
    0x48, 0x83, 0xF9, 0x40,                 /* cmp  rcx, 64             */ \
    0x48, 0x0F, 0x43, 0xD8,                 /* cmovae rbx, rax          */ \
    0xC3                                    /* ret                      */
static const uint8_t p_lshift[] = { SHIFT(0xE3) };  /* shl */
static const uint8_t p_rshift[] = { SHIFT(0xEB) };  /* shr */
#undef SHIFT

/* ( n -- ) Add n to the loop index. The loop ends when the index crosses
 * the boundary between limit-1 and limit in either direction: biasing
 * index-limit by 2^63 puts that boundary where a signed add overflows,
 * so the caller follows the call with `jno <loop body>`. Only the final
 * add sets flags; mov, lea and ret leave them alone. */
static const uint8_t p_plus_loop[] = {
    0x49, 0x8B, 0x04, 0x24,                 /* mov  rax, [r12]          */
    0x48, 0x8D, 0x14, 0x18,                 /* lea  rdx, [rax+rbx]      */
    0x49, 0x89, 0x14, 0x24,                 /* mov  [r12], rdx          */
    0x49, 0x2B, 0x44, 0x24, 0x08,           /* sub  rax, [r12+8]        */
    0x48, 0x0F, 0xBA, 0xF8, 0x3F,           /* btc  rax, 63             */
    0x48, 0x01, 0xD8,                       /* add  rax, rbx            */
    0x48, 0x8B, 0x5D, 0x00,                 /* mov  rbx, [rbp]          */
    0x48, 0x8D, 0x6D, 0x08,                 /* lea  rbp, [rbp+8]        */
    0xC3,                                   /* ret                      */
};

/* ---- Control flow (compile-time words) ---------------------------------- */

static forth_status push_control(forth_ctx *ctx, int kind, size_t pos)
{
    if (ctx->ncontrol == FORTH_MAX_CONTROL) {
        return forth_fail(ctx, FORTH_ERR_CONTROL_DEPTH,
                          "control structures nested more than %d deep",
                          FORTH_MAX_CONTROL);
    }
    forth_control *c = &ctx->control[ctx->ncontrol++];
    c->kind = kind;
    c->pos = (uint32_t)pos;
    c->line = ctx->tok_line;
    c->column = ctx->tok_column;
    return FORTH_OK;
}

/* Pop a control entry of the given kind, or fail naming what was wanted. */
static forth_status pop_control(forth_ctx *ctx, int kind, const char *opener,
                                forth_control *out)
{
    if (ctx->ncontrol == 0 || ctx->control[ctx->ncontrol - 1].kind != kind) {
        return forth_fail(ctx, FORTH_ERR_CONTROL_MISMATCH,
                          "'%.*s' without a matching '%s'",
                          (int)ctx->tok_len, ctx->tok, opener);
    }
    *out = ctx->control[--ctx->ncontrol];
    return FORTH_OK;
}

static forth_status c_if(forth_ctx *ctx, const forth_word *w)
{
    size_t fixup;
    forth_status st;
    (void)w;
    if ((st = forth_emit_0branch(ctx, 0, &fixup)) != FORTH_OK) {
        return st;
    }
    return push_control(ctx, FORTH_CF_IF, fixup);
}

static forth_status c_else(forth_ctx *ctx, const forth_word *w)
{
    forth_control orig = { 0 };
    size_t fixup;
    forth_status st;
    (void)w;
    if ((st = pop_control(ctx, FORTH_CF_IF, "if", &orig)) != FORTH_OK ||
        (st = forth_emit_jmp(ctx, 0, &fixup)) != FORTH_OK) {
        return st;
    }
    forth_patch(ctx, orig.pos, ctx->len);
    return push_control(ctx, FORTH_CF_IF, fixup);
}

static forth_status c_then(forth_ctx *ctx, const forth_word *w)
{
    forth_control orig = { 0 };
    forth_status st;
    (void)w;
    if ((st = pop_control(ctx, FORTH_CF_IF, "if", &orig)) != FORTH_OK) {
        return st;
    }
    forth_patch(ctx, orig.pos, ctx->len);
    return FORTH_OK;
}

static forth_status c_begin(forth_ctx *ctx, const forth_word *w)
{
    (void)w;
    return push_control(ctx, FORTH_CF_BEGIN, ctx->len);
}

static forth_status c_until(forth_ctx *ctx, const forth_word *w)
{
    forth_control dest = { 0 };
    forth_status st;
    (void)w;
    if ((st = pop_control(ctx, FORTH_CF_BEGIN, "begin", &dest)) != FORTH_OK) {
        return st;
    }
    return forth_emit_0branch(ctx, dest.pos, NULL);
}

static forth_status c_while(forth_ctx *ctx, const forth_word *w)
{
    size_t fixup;
    forth_status st;
    (void)w;
    if (ctx->ncontrol == 0 ||
        ctx->control[ctx->ncontrol - 1].kind != FORTH_CF_BEGIN) {
        return forth_fail(ctx, FORTH_ERR_CONTROL_MISMATCH,
                          "'while' without a matching 'begin'");
    }
    if ((st = forth_emit_0branch(ctx, 0, &fixup)) != FORTH_OK) {
        return st;
    }
    return push_control(ctx, FORTH_CF_WHILE, fixup);
}

static forth_status c_repeat(forth_ctx *ctx, const forth_word *w)
{
    forth_control orig = { 0 }, dest = { 0 };
    forth_status st;
    (void)w;
    if ((st = pop_control(ctx, FORTH_CF_WHILE, "while", &orig)) != FORTH_OK ||
        (st = pop_control(ctx, FORTH_CF_BEGIN, "begin", &dest)) != FORTH_OK ||
        (st = forth_emit_jmp(ctx, dest.pos, NULL)) != FORTH_OK) {
        return st;
    }
    forth_patch(ctx, orig.pos, ctx->len);
    return FORTH_OK;
}

static forth_status c_do(forth_ctx *ctx, const forth_word *w)
{
    forth_status st;
    (void)w;
    if ((st = forth_emit_call(ctx, (size_t)forth_word_offset(
             forth_word_lookup("(do)", 4, 1)))) != FORTH_OK) {
        return st;
    }
    return push_control(ctx, FORTH_CF_DO, ctx->len);
}

/* Discard a loop frame (what other Forths call UNLOOP). */
static const uint8_t unloop[] = {
    0x4D, 0x8D, 0x64, 0x24, 0x10,           /* lea  r12, [r12+16]       */
};

/* Close a do loop: call the hidden step helper `helper`, branch back to
 * the body on condition `jcc` (2 opcode bytes), then drop the frame. */
static forth_status close_loop(forth_ctx *ctx, const char *helper,
                               const uint8_t *jcc)
{
    forth_control dest = { 0 };
    forth_status st;
    const forth_word *h = forth_word_lookup(helper, strlen(helper), 1);
    if ((st = pop_control(ctx, FORTH_CF_DO, "do", &dest)) != FORTH_OK ||
        (st = forth_emit_call(ctx, (size_t)forth_word_offset(h))) !=
            FORTH_OK ||
        (st = forth_emit_rel32(ctx, jcc, 2, dest.pos, NULL)) != FORTH_OK) {
        return st;
    }
    return forth_emit(ctx, unloop, sizeof unloop);
}

static forth_status c_loop(forth_ctx *ctx, const forth_word *w)
{
    static const uint8_t jne[] = { 0x0F, 0x85 };     /* jne rel32 */
    (void)w;
    return close_loop(ctx, "(loop)", jne);
}

static forth_status c_plus_loop(forth_ctx *ctx, const forth_word *w)
{
    static const uint8_t jno[] = { 0x0F, 0x81 };     /* jno rel32 */
    (void)w;
    return close_loop(ctx, "(+loop)", jno);
}

/* Fail unless compiling a colon definition. */
static forth_status need_def(forth_ctx *ctx)
{
    if (ctx->in_def) {
        return FORTH_OK;
    }
    return forth_fail(ctx, FORTH_ERR_CONTROL_MISMATCH,
                      "'%.*s' outside a colon definition",
                      (int)ctx->tok_len, ctx->tok);
}

/* Return from the current definition, first dropping the frame of every
 * do loop it is inside, so `exit` in a loop needs no `unloop`. */
static forth_status c_exit(forth_ctx *ctx, const forth_word *w)
{
    static const uint8_t ret[] = { 0xC3 };
    forth_status st;
    (void)w;
    if ((st = need_def(ctx)) != FORTH_OK) {
        return st;
    }
    for (size_t i = 0; i < ctx->ncontrol; i++) {
        if (ctx->control[i].kind == FORTH_CF_DO &&
            (st = forth_emit(ctx, unloop, sizeof unloop)) != FORTH_OK) {
            return st;
        }
    }
    return forth_emit(ctx, ret, sizeof ret);
}

/* Call the definition being compiled. */
static forth_status c_recurse(forth_ctx *ctx, const forth_word *w)
{
    forth_status st;
    (void)w;
    if ((st = need_def(ctx)) != FORTH_OK) {
        return st;
    }
    return forth_emit_call(ctx, ctx->pending.start);
}

/* ---- The table ----------------------------------------------------------
 * Library order is table order, so appending entries keeps every existing
 * primitive's offset (and thus every golden image prefix) stable. */

#define CODE(a) a, sizeof a
#define PRIM(name, bytes) { name, 0, CODE(bytes), NULL }
#define HIDDEN(name, bytes) { name, FORTH_W_HIDDEN, CODE(bytes), NULL }
#define COMPILE(name, fn) { name, 0, NULL, 0, fn }

const forth_word forth_words[] = {
    /* Stack */
    PRIM("dup", p_dup),
    PRIM("drop", p_drop),
    PRIM("swap", p_swap),
    PRIM("over", p_over),
    PRIM("rot", p_rot),
    PRIM(">r", p_to_r),
    PRIM("r>", p_r_from),
    PRIM("r@", p_r_fetch),
    /* Arithmetic */
    PRIM("+", p_plus),
    PRIM("-", p_minus),
    PRIM("*", p_star),
    PRIM("/", p_slash),
    PRIM("mod", p_mod),
    PRIM("negate", p_negate),
    PRIM("abs", p_abs),
    PRIM("min", p_min),
    PRIM("max", p_max),
    PRIM("1+", p_one_plus),
    PRIM("1-", p_one_minus),
    /* Logic and comparison */
    PRIM("and", p_and),
    PRIM("or", p_or),
    PRIM("xor", p_xor),
    PRIM("invert", p_invert),
    PRIM("=", p_eq),
    PRIM("<>", p_ne),
    PRIM("<", p_lt),
    PRIM(">", p_gt),
    PRIM("0=", p_zero_eq),
    PRIM("0<", p_zero_lt),
    /* Memory */
    PRIM("@", p_fetch),
    PRIM("!", p_store),
    PRIM("c@", p_c_fetch),
    PRIM("c!", p_c_store),
    /* Result */
    PRIM("result!", p_result_store),
    /* Loops */
    PRIM("i", p_i),
    HIDDEN("(do)", p_do),
    HIDDEN("(loop)", p_loop),
    /* Compile-time words */
    COMPILE("if", c_if),
    COMPILE("else", c_else),
    COMPILE("then", c_then),
    COMPILE("begin", c_begin),
    COMPILE("until", c_until),
    COMPILE("while", c_while),
    COMPILE("repeat", c_repeat),
    COMPILE("do", c_do),
    COMPILE("loop", c_loop),
    COMPILE(":", forth_compile_colon),
    COMPILE(";", forth_compile_semicolon),
    /* Slice 2 (SINT-104): appended so earlier offsets stay put. */
    PRIM("j", p_j),
    PRIM("lshift", p_lshift),
    PRIM("rshift", p_rshift),
    HIDDEN("(+loop)", p_plus_loop),
    COMPILE("+loop", c_plus_loop),
    COMPILE("exit", c_exit),
    COMPILE("recurse", c_recurse),
};

const size_t forth_word_count = sizeof forth_words / sizeof forth_words[0];

/* Offset of the library within the image: after the 5-byte `jmp main`. */
#define LIBRARY_START 5u

int forth_name_eq(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (cb == '\0') {
            return 0;
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return b[i] == '\0';
}

const forth_word *forth_word_lookup(const char *name, size_t len,
                                    int include_hidden)
{
    for (size_t i = 0; i < forth_word_count; i++) {
        const forth_word *w = &forth_words[i];
        if ((w->flags & FORTH_W_HIDDEN) && !include_hidden) {
            continue;
        }
        if (forth_name_eq(name, len, w->name)) {
            return w;
        }
    }
    return NULL;
}

long forth_word_offset(const forth_word *w)
{
    size_t off = LIBRARY_START;
    if (w == NULL || w->code == NULL) {
        return -1;
    }
    for (size_t i = 0; i < forth_word_count && &forth_words[i] != w; i++) {
        off += forth_words[i].code_len;
    }
    return (long)off;
}

size_t forth_library_size(void)
{
    size_t size = 0;
    for (size_t i = 0; i < forth_word_count; i++) {
        size += forth_words[i].code_len;
    }
    return size;
}

long forth_primitive_offset(const char *name)
{
    size_t len = 0;
    if (name == NULL) {
        return -1;
    }
    while (name[len] != '\0') {
        len++;
    }
    return forth_word_offset(forth_word_lookup(name, len, 1));
}
