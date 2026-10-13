/*
 * internal.h — definitions shared between the compiler's source files.
 * Not part of the public API; the tests include it for white-box checks.
 */
#ifndef SINTER_FORTH_INTERNAL_H
#define SINTER_FORTH_INTERNAL_H

#include "sinter_forth.h"

/* ---- Word table -------------------------------------------------------- */

/* Word flags. */
#define FORTH_W_HIDDEN 0x1u  /* internal helper: in the library but not
                                findable by name from Forth source       */

typedef struct forth_word forth_word;

/* Compile-time action for words that are not simply "call my code".
 * Control-flow words and `:`/`;` use this. */
typedef forth_status (*forth_compile_fn)(forth_ctx *ctx, const forth_word *w);

struct forth_word {
    const char *name;
    unsigned flags;
    const uint8_t *code;     /* library routine, ending in ret; or NULL */
    size_t code_len;
    forth_compile_fn compile; /* NULL: compile as `call <code>`          */
};

/* The table itself, in library order (words.c). */
extern const forth_word forth_words[];
extern const size_t forth_word_count;

/* Find a word by name, case-insensitively. Hidden words are found only
 * when `include_hidden` is non-zero. */
const forth_word *forth_word_lookup(const char *name, size_t len,
                                    int include_hidden);

/* Image offset of a word's library code, or -1 if it has none. */
long forth_word_offset(const forth_word *w);

/* Case-insensitive comparison of a length-delimited name with a C string. */
int forth_name_eq(const char *a, size_t alen, const char *b);

/* ---- Control-flow kinds (forth_control.kind) --------------------------- */
enum {
    FORTH_CF_IF = 1,   /* pos: rel32 field of a forward branch (if/else) */
    FORTH_CF_BEGIN,    /* pos: backward branch target                    */
    FORTH_CF_WHILE,    /* pos: rel32 field of while's forward branch     */
    FORTH_CF_DO        /* pos: loop body start (backward branch target)  */
};

/* ---- Emitters (emit.c) -------------------------------------------------
 * All return FORTH_OK or FORTH_ERR_OUTPUT_FULL (with ctx->error filled). */
forth_status forth_emit(forth_ctx *ctx, const uint8_t *bytes, size_t n);
/* Emit `op` (1 or 2 opcode bytes) followed by a rel32 to `target`; or, if
 * `fixup` is non-NULL, a zero placeholder whose position is stored there
 * for a later forth_patch(). */
forth_status forth_emit_rel32(forth_ctx *ctx, const uint8_t *op, size_t oplen,
                              size_t target, size_t *fixup);
forth_status forth_emit_call(forth_ctx *ctx, size_t target);
forth_status forth_emit_literal(forth_ctx *ctx, int64_t value);
/* Pop the data stack; branch if the popped value was zero. Emits a rel32
 * to `target`, or a placeholder if `fixup` is non-NULL (its position is
 * stored there for forth_patch()). */
forth_status forth_emit_0branch(forth_ctx *ctx, size_t target, size_t *fixup);
forth_status forth_emit_jmp(forth_ctx *ctx, size_t target, size_t *fixup);
/* Point the rel32 field at `fixup` at `target`. */
void forth_patch(forth_ctx *ctx, size_t fixup, size_t target);
forth_status forth_emit_prologue(forth_ctx *ctx);
forth_status forth_emit_epilogue(forth_ctx *ctx);
forth_status forth_emit_guard(forth_ctx *ctx);

/* ---- Error reporting (compiler.c) -------------------------------------- */
/* Record an error at the current token and return `status`. */
forth_status forth_fail(forth_ctx *ctx, forth_status status,
                        const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* ---- Compile-time actions for `:` and `;` (compiler.c) ----------------- */
forth_status forth_compile_colon(forth_ctx *ctx, const forth_word *w);
forth_status forth_compile_semicolon(forth_ctx *ctx, const forth_word *w);

#endif /* SINTER_FORTH_INTERNAL_H */
