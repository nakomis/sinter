/*
 * compiler.c — tokeniser, dictionary and one-pass compile driver.
 *
 * The whole compiler is a single loop: read a whitespace-delimited token;
 * if it names a colon definition, emit a call to it; else if it is in the
 * word table, emit a call to its library code or run its compile-time
 * action; else if it parses as a number, emit a literal push; else fail.
 * Forward branches are patched through a small control-flow stack.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

/* ---- Errors ------------------------------------------------------------ */

forth_status forth_fail(forth_ctx *ctx, forth_status status,
                        const char *fmt, ...)
{
    char *msg = ctx->error.message;
    size_t used = 0;
    va_list ap;

    /* Keep the first error: later ones are usually consequences of it. */
    if (ctx->error.status != FORTH_OK) {
        return ctx->error.status;
    }
    ctx->error.status = status;
    ctx->error.line = ctx->tok_line;
    ctx->error.column = ctx->tok_column;
    if (ctx->tok_line != 0) {
        int n = snprintf(msg, FORTH_MAX_MESSAGE, "line %u, column %u: ",
                         ctx->tok_line, ctx->tok_column);
        used = n > 0 && n < FORTH_MAX_MESSAGE ? (size_t)n : 0;
    }
    va_start(ap, fmt);
    vsnprintf(msg + used, FORTH_MAX_MESSAGE - used, fmt, ap);
    va_end(ap);
    return status;
}

const char *forth_status_name(forth_status status)
{
    switch (status) {
    case FORTH_OK:                       return "ok";
    case FORTH_ERR_ARGUMENT:             return "bad-argument";
    case FORTH_ERR_UNKNOWN_WORD:         return "unknown-word";
    case FORTH_ERR_NUMBER_RANGE:         return "number-range";
    case FORTH_ERR_CONTROL_MISMATCH:     return "control-mismatch";
    case FORTH_ERR_CONTROL_UNCLOSED:     return "control-unclosed";
    case FORTH_ERR_CONTROL_DEPTH:        return "control-depth";
    case FORTH_ERR_NESTED_DEFINITION:    return "nested-definition";
    case FORTH_ERR_STRAY_SEMICOLON:      return "stray-semicolon";
    case FORTH_ERR_UNTERMINATED_DEF:     return "unterminated-definition";
    case FORTH_ERR_MISSING_NAME:         return "missing-name";
    case FORTH_ERR_BAD_NAME:             return "bad-name";
    case FORTH_ERR_TOO_MANY_DEFS:        return "too-many-definitions";
    case FORTH_ERR_UNTERMINATED_COMMENT: return "unterminated-comment";
    case FORTH_ERR_OUTPUT_FULL:          return "output-full";
    }
    return "unknown-status";
}

/* ---- Tokeniser --------------------------------------------------------- */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static void advance(forth_ctx *ctx)
{
    if (ctx->src[ctx->pos] == '\n') {
        ctx->line++;
        ctx->column = 1;
    } else {
        ctx->column++;
    }
    ctx->pos++;
}

/* Read the next whitespace-delimited token into ctx->tok. Returns 0 at end
 * of input. */
static int next_token(forth_ctx *ctx)
{
    while (ctx->pos < ctx->src_len && is_space(ctx->src[ctx->pos])) {
        advance(ctx);
    }
    if (ctx->pos >= ctx->src_len) {
        return 0;
    }
    ctx->tok = ctx->src + ctx->pos;
    ctx->tok_line = ctx->line;
    ctx->tok_column = ctx->column;
    while (ctx->pos < ctx->src_len && !is_space(ctx->src[ctx->pos])) {
        advance(ctx);
    }
    ctx->tok_len = (size_t)(ctx->src + ctx->pos - ctx->tok);
    return 1;
}

/* Comments: `( ... )` and `\ ...` to end of line. Both introducers are
 * whole tokens, as in standard Forth, so `(foo)` is an ordinary word. */
static forth_status skip_comment(forth_ctx *ctx, int *skipped)
{
    *skipped = 0;
    if (ctx->tok_len != 1) {
        return FORTH_OK;
    }
    if (ctx->tok[0] == '\\') {
        while (ctx->pos < ctx->src_len && ctx->src[ctx->pos] != '\n') {
            advance(ctx);
        }
        *skipped = 1;
    } else if (ctx->tok[0] == '(') {
        while (ctx->pos < ctx->src_len && ctx->src[ctx->pos] != ')') {
            advance(ctx);
        }
        if (ctx->pos >= ctx->src_len) {
            return forth_fail(ctx, FORTH_ERR_UNTERMINATED_COMMENT,
                              "'(' comment is never closed with ')'");
        }
        advance(ctx);
        *skipped = 1;
    }
    return FORTH_OK;
}

/* Parse a literal: optional '-', then decimal digits, or hex introduced by
 * '$' or '0x'. Returns 1 and sets *value if the token is a number at all
 * (possibly out of range, reported via *range_ok), 0 if it is not. */
static int parse_number(const char *s, size_t len, int64_t *value,
                        int *range_ok)
{
    size_t i = 0;
    int neg = 0, base = 10;
    uint64_t v = 0, limit;

    *range_ok = 1;
    if (i < len && s[i] == '-') {
        neg = 1;
        i++;
    }
    if (i < len && s[i] == '$') {
        base = 16;
        i++;
    } else if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' ||
                                               s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i == len) {
        return 0;
    }
    /* Hex may use the full 64 bits; decimal must fit in a signed cell. */
    limit = base == 16 ? UINT64_MAX
          : neg        ? (uint64_t)INT64_MAX + 1u
                       : (uint64_t)INT64_MAX;
    for (; i < len; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            return 0;
        }
        if (v > (limit - d) / (unsigned)base) {
            *range_ok = 0;
        } else {
            v = v * (unsigned)base + d;
        }
    }
    *value = (int64_t)(neg ? (uint64_t)0 - v : v);
    return 1;
}

/* ---- Dictionary -------------------------------------------------------- */

static const forth_def *find_def(const forth_ctx *ctx, const char *name,
                                 size_t len)
{
    for (size_t i = ctx->ndefs; i-- > 0;) {
        if (forth_name_eq(name, len, ctx->defs[i].name)) {
            return &ctx->defs[i];
        }
    }
    return NULL;
}

const forth_def *forth_find_def(const forth_ctx *ctx, const char *name)
{
    if (ctx == NULL || name == NULL) {
        return NULL;
    }
    return find_def(ctx, name, strlen(name));
}

/* ---- `:` and `;` ------------------------------------------------------- */

forth_status forth_compile_colon(forth_ctx *ctx, const forth_word *w)
{
    unsigned line = ctx->tok_line, column = ctx->tok_column;
    const forth_word *clash;
    forth_status st;

    (void)w;
    if (ctx->in_def) {
        return forth_fail(ctx, FORTH_ERR_NESTED_DEFINITION,
                          "':' inside the definition of '%s' (missing ';'?)",
                          ctx->pending.name);
    }
    if (ctx->ncontrol != 0) {
        return forth_fail(ctx, FORTH_ERR_NESTED_DEFINITION,
                          "':' inside an open control structure");
    }
    if (!next_token(ctx)) {
        ctx->tok_line = line;
        ctx->tok_column = column;
        return forth_fail(ctx, FORTH_ERR_MISSING_NAME,
                          "':' must be followed by a name");
    }
    if (ctx->tok_len > FORTH_MAX_NAME) {
        return forth_fail(ctx, FORTH_ERR_BAD_NAME,
                          "name '%.*s...' is longer than %d characters",
                          16, ctx->tok, FORTH_MAX_NAME);
    }
    /* Redefining an ordinary word is allowed (the new one shadows it), but
     * redefining a compile-time word would make the source unreadable. */
    clash = forth_word_lookup(ctx->tok, ctx->tok_len, 0);
    if (clash != NULL && clash->compile != NULL) {
        return forth_fail(ctx, FORTH_ERR_BAD_NAME,
                          "'%s' is a reserved word and cannot be redefined",
                          clash->name);
    }
    if (ctx->ndefs == FORTH_MAX_DEFS) {
        return forth_fail(ctx, FORTH_ERR_TOO_MANY_DEFS,
                          "more than %d colon definitions", FORTH_MAX_DEFS);
    }

    /* Definitions are compiled in line with the top-level code, so jump
     * over the body; `;` patches this jump. */
    if ((st = forth_emit_jmp(ctx, 0, &ctx->def_skip)) != FORTH_OK) {
        return st;
    }
    memset(&ctx->pending, 0, sizeof ctx->pending);
    memcpy(ctx->pending.name, ctx->tok, ctx->tok_len);
    ctx->pending.start = (uint32_t)ctx->len;
    ctx->in_def = 1;
    return FORTH_OK;
}

/* Report the innermost open control structure as unclosed. */
static forth_status fail_unclosed(forth_ctx *ctx, const char *where)
{
    static const char *const names[] = { "?", "if", "begin", "while", "do" };
    const forth_control *c = &ctx->control[ctx->ncontrol - 1];
    return forth_fail(ctx, FORTH_ERR_CONTROL_UNCLOSED,
                      "'%s' at line %u, column %u is not closed before %s",
                      names[c->kind], c->line, c->column, where);
}

forth_status forth_compile_semicolon(forth_ctx *ctx, const forth_word *w)
{
    static const uint8_t ret[] = { 0xC3 };
    forth_status st;

    (void)w;
    if (!ctx->in_def) {
        return forth_fail(ctx, FORTH_ERR_STRAY_SEMICOLON,
                          "';' outside a colon definition");
    }
    if (ctx->ncontrol != 0) {
        return fail_unclosed(ctx, "';'");
    }
    if ((st = forth_emit(ctx, ret, sizeof ret)) != FORTH_OK) {
        return st;
    }
    forth_patch(ctx, ctx->def_skip, ctx->len);
    ctx->pending.end = (uint32_t)ctx->len;
    ctx->defs[ctx->ndefs++] = ctx->pending;
    ctx->in_def = 0;
    return FORTH_OK;
}

/* ---- Driver ------------------------------------------------------------ */

static forth_status compile_token(forth_ctx *ctx)
{
    const forth_def *def;
    const forth_word *w;
    int64_t value;
    int range_ok;

    /* User definitions first, newest first, so they can shadow primitives. */
    def = find_def(ctx, ctx->tok, ctx->tok_len);
    if (def != NULL) {
        return forth_emit_call(ctx, def->start);
    }
    w = forth_word_lookup(ctx->tok, ctx->tok_len, 0);
    if (w != NULL) {
        if (w->compile != NULL) {
            return w->compile(ctx, w);
        }
        return forth_emit_call(ctx, (size_t)forth_word_offset(w));
    }
    if (parse_number(ctx->tok, ctx->tok_len, &value, &range_ok)) {
        if (!range_ok) {
            return forth_fail(ctx, FORTH_ERR_NUMBER_RANGE,
                              "number '%.*s' does not fit in a 64-bit cell",
                              (int)(ctx->tok_len > 40 ? 40 : ctx->tok_len),
                              ctx->tok);
        }
        return forth_emit_literal(ctx, value);
    }
    return forth_fail(ctx, FORTH_ERR_UNKNOWN_WORD, "unknown word '%.*s'",
                      (int)(ctx->tok_len > 40 ? 40 : ctx->tok_len), ctx->tok);
}

/* Emit the fixed image header: `jmp main` then the primitive library. */
static forth_status emit_library(forth_ctx *ctx, size_t *main_fixup)
{
    forth_status st = forth_emit_jmp(ctx, 0, main_fixup);
    for (size_t i = 0; st == FORTH_OK && i < forth_word_count; i++) {
        if (forth_words[i].code != NULL) {
            st = forth_emit(ctx, forth_words[i].code, forth_words[i].code_len);
        }
    }
    return st;
}

forth_status forth_compile(forth_ctx *ctx, const char *src, size_t src_len,
                           uint8_t *buf, size_t buf_cap)
{
    size_t main_fixup;
    forth_status st;

    if (ctx == NULL) {
        return FORTH_ERR_ARGUMENT;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->src = src;
    ctx->src_len = src_len;
    ctx->line = 1;
    ctx->column = 1;
    ctx->out = buf;
    ctx->cap = buf_cap;
    if ((src == NULL && src_len != 0) || buf == NULL) {
        return forth_fail(ctx, FORTH_ERR_ARGUMENT,
                          "NULL source or output buffer");
    }

    if ((st = emit_library(ctx, &main_fixup)) != FORTH_OK) {
        return st;
    }
    ctx->main_offset = ctx->len;
    forth_patch(ctx, main_fixup, ctx->main_offset);
    if ((st = forth_emit_prologue(ctx)) != FORTH_OK) {
        return st;
    }

    while (next_token(ctx)) {
        int skipped;
        if ((st = skip_comment(ctx, &skipped)) != FORTH_OK) {
            return st;
        }
        if (!skipped && (st = compile_token(ctx)) != FORTH_OK) {
            return st;
        }
    }

    /* End of input: report what was left open at the end of the source. */
    ctx->tok_line = ctx->line;
    ctx->tok_column = ctx->column;
    if (ctx->in_def) {
        if (ctx->ncontrol != 0) {
            return fail_unclosed(ctx, "the end of the input");
        }
        return forth_fail(ctx, FORTH_ERR_UNTERMINATED_DEF,
                          "definition of '%s' is missing its ';'",
                          ctx->pending.name);
    }
    if (ctx->ncontrol != 0) {
        return fail_unclosed(ctx, "the end of the input");
    }

    if ((st = forth_emit_epilogue(ctx)) != FORTH_OK ||
        (st = forth_emit_guard(ctx)) != FORTH_OK) {
        return st;
    }
    ctx->code_len = ctx->len;
    return FORTH_OK;
}
