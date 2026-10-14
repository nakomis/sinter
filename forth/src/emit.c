/*
 * emit.c — the only place outside words.c that knows x86-64 encodings.
 *
 * The compiler itself only ever produces calls, literal pushes, branches
 * and the main prologue/epilogue; everything else lives in the primitive
 * library. Every encoding here has been checked against GNU objdump, and
 * the golden tests pin them down.
 */
#include <string.h>

#include "internal.h"

forth_status forth_emit(forth_ctx *ctx, const uint8_t *bytes, size_t n)
{
    if (n > ctx->cap - ctx->len) {
        return forth_fail(ctx, FORTH_ERR_OUTPUT_FULL,
                          "output buffer full (%zu bytes); program too large",
                          ctx->cap);
    }
    memcpy(ctx->out + ctx->len, bytes, n);
    ctx->len += n;
    return FORTH_OK;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* rel32 from the end of a field at `field` (i.e. field + 4) to `target`.
 * Images are far smaller than 2 GiB, so this never overflows. */
static uint32_t rel32(size_t field, size_t target)
{
    return (uint32_t)((int64_t)target - (int64_t)(field + 4));
}

void forth_patch(forth_ctx *ctx, size_t fixup, size_t target)
{
    put_le32(ctx->out + fixup, rel32(fixup, target));
}

forth_status forth_emit_rel32(forth_ctx *ctx, const uint8_t *op,
                             size_t oplen, size_t target, size_t *fixup)
{
    uint8_t buf[6];
    size_t field = ctx->len + oplen;

    memcpy(buf, op, oplen);
    put_le32(buf + oplen, fixup ? 0 : rel32(field, target));
    if (fixup) {
        *fixup = field;
    }
    return forth_emit(ctx, buf, oplen + 4);
}

forth_status forth_emit_call(forth_ctx *ctx, size_t target)
{
    static const uint8_t op[] = { 0xE8 };            /* call rel32 */
    return forth_emit_rel32(ctx, op, sizeof op, target, NULL);
}

forth_status forth_emit_jmp(forth_ctx *ctx, size_t target, size_t *fixup)
{
    static const uint8_t op[] = { 0xE9 };            /* jmp rel32 */
    return forth_emit_rel32(ctx, op, sizeof op, target, fixup);
}

forth_status forth_emit_0branch(forth_ctx *ctx, size_t target, size_t *fixup)
{
    /* mov and lea leave the flags from `test` intact, so the stack is
     * popped between the test and the branch. */
    static const uint8_t pop_test[] = {
        0x48, 0x85, 0xDB,                /* test rbx, rbx       */
        0x48, 0x8B, 0x5D, 0x00,          /* mov  rbx, [rbp]     */
        0x48, 0x8D, 0x6D, 0x08,          /* lea  rbp, [rbp+8]   */
    };
    static const uint8_t op[] = { 0x0F, 0x84 };      /* jz rel32 */
    forth_status st = forth_emit(ctx, pop_test, sizeof pop_test);
    if (st != FORTH_OK) {
        return st;
    }
    return forth_emit_rel32(ctx, op, sizeof op, target, fixup);
}

forth_status forth_emit_literal(forth_ctx *ctx, int64_t value)
{
    uint8_t buf[18] = {
        0x48, 0x83, 0xED, 0x08,          /* sub  rbp, 8         */
        0x48, 0x89, 0x5D, 0x00,          /* mov  [rbp], rbx     */
    };
    size_t n = 8;

    if (value >= 0 && value <= (int64_t)UINT32_MAX) {
        buf[n++] = 0xBB;                 /* mov ebx, imm32 (zero-extends) */
        put_le32(buf + n, (uint32_t)value);
        n += 4;
    } else if (value >= INT32_MIN && value < 0) {
        buf[n++] = 0x48;                 /* mov rbx, simm32 (sign-extends) */
        buf[n++] = 0xC7;
        buf[n++] = 0xC3;
        put_le32(buf + n, (uint32_t)value);
        n += 4;
    } else {
        buf[n++] = 0x48;                 /* movabs rbx, imm64 */
        buf[n++] = 0xBB;
        put_le64(buf + n, (uint64_t)value);
        n += 8;
    }
    return forth_emit(ctx, buf, n);
}

/* The offsets 0x08, 0x20, 0xC00 and 0x1000 below are FORTH_REG_DONE,
 * FORTH_REG_FLAGS, FORTH_RSTACK_TOP and FORTH_DSTACK_TOP. */
forth_status forth_emit_prologue(forth_ctx *ctx)
{
    static const uint8_t code[] = {
        0x53,                                     /* push rbx              */
        0x55,                                     /* push rbp              */
        0x41, 0x54,                               /* push r12              */
        0x41, 0x55,                               /* push r13              */
        0x49, 0x89, 0xFD,                         /* mov  r13, rdi         */
        0x49, 0x8D, 0xAD, 0x00, 0x10, 0x00, 0x00, /* lea  rbp, [r13+0x1000] */
        0x4D, 0x8D, 0xA5, 0x00, 0x0C, 0x00, 0x00, /* lea  r12, [r13+0xc00] */
        0x31, 0xDB,                               /* xor  ebx, ebx         */
        0x31, 0xC0,                               /* xor  eax, eax         */
        0x49, 0x89, 0x45, 0x08,                   /* mov  [r13+0x08], rax  */
        0x49, 0x89, 0x45, 0x20,                   /* mov  [r13+0x20], rax  */
    };
    return forth_emit(ctx, code, sizeof code);
}

/* 0x10 and 0x18 are FORTH_REG_DEPTH and FORTH_REG_TOS. */
forth_status forth_emit_epilogue(forth_ctx *ctx)
{
    static const uint8_t code[] = {
        0x49, 0x89, 0x5D, 0x18,                   /* mov  [r13+0x18], rbx  */
        0x49, 0x8D, 0x85, 0x00, 0x10, 0x00, 0x00, /* lea  rax, [r13+0x1000] */
        0x48, 0x29, 0xE8,                         /* sub  rax, rbp         */
        0x48, 0xC1, 0xF8, 0x03,                   /* sar  rax, 3           */
        0x49, 0x89, 0x45, 0x10,                   /* mov  [r13+0x10], rax  */
        0x41, 0x5D,                               /* pop  r13              */
        0x41, 0x5C,                               /* pop  r12              */
        0x5D,                                     /* pop  rbp              */
        0x5B,                                     /* pop  rbx              */
        0xC3,                                     /* ret                   */
    };
    return forth_emit(ctx, code, sizeof code);
}

forth_status forth_emit_guard(forth_ctx *ctx)
{
    uint8_t ud2[FORTH_GUARD_BYTES];
    for (size_t i = 0; i < sizeof ud2; i += 2) {
        ud2[i] = 0x0F;                            /* ud2 */
        ud2[i + 1] = 0x0B;
    }
    return forth_emit(ctx, ud2, sizeof ud2);
}
