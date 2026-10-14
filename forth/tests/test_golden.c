/*
 * test_golden.c — byte-exact checks of everything the compiler emits:
 * each primitive's library code, the call/literal/branch sequences, every
 * control-flow shape, and whole images against checked-in golden files.
 *
 * The expected bytes here were produced by assembling the commented
 * instructions with GNU as and reading them back with objdump; the disasm
 * suite re-checks them against objdump on every run where it is present.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* ---- Primitive encodings ----------------------------------------------- */

#define CMP(cc) "31 c0 48 39 5d 00 0f " cc " c0 48 f7 d8 48 89 c3 48 83 c5 08 c3"
#define CMP_ASM(cc) "xor eax,eax|cmp QWORD PTR [rbp+0x0],rbx|" cc " al|" \
                    "neg rax|mov rbx,rax|add rbp,0x8|ret"

const prim_golden prim_goldens[] = {
    { "dup", "48 83 ed 08 48 89 5d 00 c3",
      "sub rbp,0x8|mov QWORD PTR [rbp+0x0],rbx|ret" },
    { "drop", "48 8b 5d 00 48 83 c5 08 c3",
      "mov rbx,QWORD PTR [rbp+0x0]|add rbp,0x8|ret" },
    { "swap", "48 8b 45 00 48 89 5d 00 48 89 c3 c3",
      "mov rax,QWORD PTR [rbp+0x0]|mov QWORD PTR [rbp+0x0],rbx|"
      "mov rbx,rax|ret" },
    { "over", "48 83 ed 08 48 89 5d 00 48 8b 5d 08 c3",
      "sub rbp,0x8|mov QWORD PTR [rbp+0x0],rbx|mov rbx,QWORD PTR [rbp+0x8]|"
      "ret" },
    { "rot", "48 8b 45 08 48 8b 4d 00 48 89 4d 08 48 89 5d 00 48 89 c3 c3",
      "mov rax,QWORD PTR [rbp+0x8]|mov rcx,QWORD PTR [rbp+0x0]|"
      "mov QWORD PTR [rbp+0x8],rcx|mov QWORD PTR [rbp+0x0],rbx|"
      "mov rbx,rax|ret" },
    { ">r", "49 83 ec 08 49 89 1c 24 48 8b 5d 00 48 83 c5 08 c3",
      "sub r12,0x8|mov QWORD PTR [r12],rbx|mov rbx,QWORD PTR [rbp+0x0]|"
      "add rbp,0x8|ret" },
    { "r>", "48 83 ed 08 48 89 5d 00 49 8b 1c 24 49 83 c4 08 c3",
      "sub rbp,0x8|mov QWORD PTR [rbp+0x0],rbx|mov rbx,QWORD PTR [r12]|"
      "add r12,0x8|ret" },
    { "r@", "48 83 ed 08 48 89 5d 00 49 8b 1c 24 c3",
      "sub rbp,0x8|mov QWORD PTR [rbp+0x0],rbx|mov rbx,QWORD PTR [r12]|ret" },
    { "+", "48 03 5d 00 48 83 c5 08 c3",
      "add rbx,QWORD PTR [rbp+0x0]|add rbp,0x8|ret" },
    { "-", "48 8b 45 00 48 29 d8 48 89 c3 48 83 c5 08 c3",
      "mov rax,QWORD PTR [rbp+0x0]|sub rax,rbx|mov rbx,rax|add rbp,0x8|ret" },
    { "*", "48 0f af 5d 00 48 83 c5 08 c3",
      "imul rbx,QWORD PTR [rbp+0x0]|add rbp,0x8|ret" },
    { "/", "48 8b 45 00 48 83 c5 08 48 85 db 74 16 48 83 fb ff 74 09 48 99 "
           "48 f7 fb 48 89 c3 c3 48 f7 d8 48 89 c3 c3 49 83 4d 20 01 31 db c3",
      "mov rax,QWORD PTR [rbp+0x0]|add rbp,0x8|test rbx,rbx|je 0x23|"
      "cmp rbx,0xffffffffffffffff|je 0x1c|cqo|idiv rbx|mov rbx,rax|ret|"
      "neg rax|mov rbx,rax|ret|or QWORD PTR [r13+0x20],0x1|xor ebx,ebx|ret" },
    { "mod", "48 8b 45 00 48 83 c5 08 48 85 db 74 12 48 83 fb ff 74 09 48 99 "
             "48 f7 fb 48 89 d3 c3 31 db c3 49 83 4d 20 01 48 89 c3 c3",
      "mov rax,QWORD PTR [rbp+0x0]|add rbp,0x8|test rbx,rbx|je 0x1f|"
      "cmp rbx,0xffffffffffffffff|je 0x1c|cqo|idiv rbx|mov rbx,rdx|ret|"
      "xor ebx,ebx|ret|or QWORD PTR [r13+0x20],0x1|mov rbx,rax|ret" },
    { "negate", "48 f7 db c3", "neg rbx|ret" },
    { "abs", "48 89 d8 48 f7 d8 48 0f 49 d8 c3",
      "mov rax,rbx|neg rax|cmovns rbx,rax|ret" },
    { "min", "48 8b 45 00 48 83 c5 08 48 39 d8 48 0f 4c d8 c3",
      "mov rax,QWORD PTR [rbp+0x0]|add rbp,0x8|cmp rax,rbx|cmovl rbx,rax|ret" },
    { "max", "48 8b 45 00 48 83 c5 08 48 39 d8 48 0f 4f d8 c3",
      "mov rax,QWORD PTR [rbp+0x0]|add rbp,0x8|cmp rax,rbx|cmovg rbx,rax|ret" },
    { "1+", "48 83 c3 01 c3", "add rbx,0x1|ret" },
    { "1-", "48 83 eb 01 c3", "sub rbx,0x1|ret" },
    { "and", "48 23 5d 00 48 83 c5 08 c3",
      "and rbx,QWORD PTR [rbp+0x0]|add rbp,0x8|ret" },
    { "or", "48 0b 5d 00 48 83 c5 08 c3",
      "or rbx,QWORD PTR [rbp+0x0]|add rbp,0x8|ret" },
    { "xor", "48 33 5d 00 48 83 c5 08 c3",
      "xor rbx,QWORD PTR [rbp+0x0]|add rbp,0x8|ret" },
    { "invert", "48 f7 d3 c3", "not rbx|ret" },
    { "=", CMP("94"), CMP_ASM("sete") },
    { "<>", CMP("95"), CMP_ASM("setne") },
    { "<", CMP("9c"), CMP_ASM("setl") },
    { ">", CMP("9f"), CMP_ASM("setg") },
    { "0=", "31 c0 48 85 db 0f 94 c0 48 f7 d8 48 89 c3 c3",
      "xor eax,eax|test rbx,rbx|sete al|neg rax|mov rbx,rax|ret" },
    { "0<", "48 c1 fb 3f c3", "sar rbx,0x3f|ret" },
    { "@", "81 e3 f8 0f 00 00 49 8b 5c 1d 00 c3",
      "and ebx,0xff8|mov rbx,QWORD PTR [r13+rbx*1+0x0]|ret" },
    { "!", "81 e3 f8 0f 00 00 48 8b 45 00 49 89 44 1d 00 48 8b 5d 08 "
           "48 83 c5 10 c3",
      "and ebx,0xff8|mov rax,QWORD PTR [rbp+0x0]|"
      "mov QWORD PTR [r13+rbx*1+0x0],rax|mov rbx,QWORD PTR [rbp+0x8]|"
      "add rbp,0x10|ret" },
    { "c@", "81 e3 ff 0f 00 00 41 0f b6 5c 1d 00 c3",
      "and ebx,0xfff|movzx ebx,BYTE PTR [r13+rbx*1+0x0]|ret" },
    { "c!", "81 e3 ff 0f 00 00 48 8b 45 00 41 88 44 1d 00 48 8b 5d 08 "
            "48 83 c5 10 c3",
      "and ebx,0xfff|mov rax,QWORD PTR [rbp+0x0]|"
      "mov BYTE PTR [r13+rbx*1+0x0],al|mov rbx,QWORD PTR [rbp+0x8]|"
      "add rbp,0x10|ret" },
    { "result!", "49 89 5d 00 49 c7 45 08 01 00 00 00 49 8b 45 08 "
                 "48 8b 5d 00 48 83 c5 08 c3",
      "mov QWORD PTR [r13+0x0],rbx|mov QWORD PTR [r13+0x8],0x1|"
      "mov rax,QWORD PTR [r13+0x8]|mov rbx,QWORD PTR [rbp+0x0]|"
      "add rbp,0x8|ret" },
    { "i", "48 83 ed 08 48 89 5d 00 49 8b 1c 24 c3",
      "sub rbp,0x8|mov QWORD PTR [rbp+0x0],rbx|mov rbx,QWORD PTR [r12]|ret" },
    { "(do)", "49 83 ec 10 49 89 1c 24 48 8b 45 00 49 89 44 24 08 "
              "48 8b 5d 08 48 83 c5 10 c3",
      "sub r12,0x10|mov QWORD PTR [r12],rbx|mov rax,QWORD PTR [rbp+0x0]|"
      "mov QWORD PTR [r12+0x8],rax|mov rbx,QWORD PTR [rbp+0x8]|"
      "add rbp,0x10|ret" },
    { "(loop)", "49 8b 04 24 48 83 c0 01 49 89 04 24 49 3b 44 24 08 c3",
      "mov rax,QWORD PTR [r12]|add rax,0x1|mov QWORD PTR [r12],rax|"
      "cmp rax,QWORD PTR [r12+0x8]|ret" },
};
const size_t prim_golden_count = sizeof prim_goldens / sizeof prim_goldens[0];

/* Prologue length: the call for the first top-level word starts here. */
#define PROLOGUE_LEN 35u

static void check_primitive_table(void)
{
    size_t covered = 0;

    for (size_t i = 0; i < prim_golden_count; i++) {
        const prim_golden *g = &prim_goldens[i];
        const forth_word *w = forth_word_lookup(g->name, strlen(g->name), 1);
        uint8_t want[64];
        size_t n = parse_hex(g->hex, want, sizeof want);

        CHECK(w != NULL && w->code != NULL, "primitive '%s' missing", g->name);
        if (w == NULL || w->code == NULL) {
            continue;
        }
        CHECK(w->code_len == n && memcmp(w->code, want, n) == 0,
              "primitive '%s' bytes differ from golden", g->name);
        CHECK(w->code[w->code_len - 1] == 0xC3,
              "primitive '%s' does not end in ret", g->name);
    }
    /* Every library primitive must have a golden entry. */
    for (size_t i = 0; i < forth_word_count; i++) {
        if (forth_words[i].code == NULL) {
            continue;
        }
        covered++;
        int found = 0;
        for (size_t j = 0; j < prim_golden_count; j++) {
            found |= strcmp(prim_goldens[j].name, forth_words[i].name) == 0;
        }
        CHECK(found, "primitive '%s' has no golden entry", forth_words[i].name);
    }
    CHECK(covered == prim_golden_count, "golden table has %zu entries, "
          "library has %zu primitives", prim_golden_count, covered);
}

/* The library is copied verbatim into every image after `jmp main`. */
static void check_library_layout(void)
{
    forth_ctx ctx;
    const uint8_t *img = compile_ok("", &ctx);
    if (img == NULL) {
        return;
    }
    CHECK(ctx.main_offset == 5 + forth_library_size(),
          "main at 0x%zx, expected after library", ctx.main_offset);
    CHECK(img[0] == 0xE9, "image does not start with jmp");
    for (size_t i = 0; i < forth_word_count; i++) {
        const forth_word *w = &forth_words[i];
        long off = forth_primitive_offset(w->name);
        if (w->code == NULL) {
            CHECK(off == -1, "'%s' has no code but an offset", w->name);
            continue;
        }
        CHECK(off > 0 && memcmp(img + off, w->code, w->code_len) == 0,
              "'%s' not found at its library offset", w->name);
    }
    /* Empty program: prologue, epilogue, guard. */
    CHECK(ctx.code_len == ctx.main_offset + PROLOGUE_LEN + 29 + 16,
          "empty program is %zu bytes", ctx.code_len);
}

static int32_t rel32_at(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* Using a primitive compiles to exactly one `call rel32` to it. */
static void check_primitive_calls(void)
{
    for (size_t i = 0; i < forth_word_count; i++) {
        const forth_word *w = &forth_words[i];
        forth_ctx ctx;
        if (w->code == NULL || (w->flags & FORTH_W_HIDDEN)) {
            continue;
        }
        const uint8_t *img = compile_ok(w->name, &ctx);
        if (img == NULL) {
            continue;
        }
        size_t at = ctx.main_offset + PROLOGUE_LEN;
        CHECK(img[at] == 0xE8, "'%s' did not compile to call", w->name);
        CHECK((long)(at + 5) + rel32_at(img + at + 1) ==
                  forth_primitive_offset(w->name),
              "'%s' call has wrong target", w->name);
        CHECK(ctx.code_len == ctx.main_offset + PROLOGUE_LEN + 5 + 29 + 16,
              "'%s' compiled to more than one call", w->name);
    }
}

/* Compile `src` and compare the main body (between prologue and
 * epilogue) with `hex`. */
static void check_body(const char *src, const char *hex)
{
    forth_ctx ctx;
    uint8_t want[128];
    size_t n = parse_hex(hex, want, sizeof want);
    const uint8_t *img = compile_ok(src, &ctx);
    if (img == NULL) {
        return;
    }
    size_t body = ctx.main_offset + PROLOGUE_LEN;
    size_t len = ctx.code_len - 29 - 16 - body;
    CHECK(len == n && memcmp(img + body, want, n) == 0,
          "body of \"%s\" differs from golden", src);
}

#define PUSH "48 83 ed 08 48 89 5d 00 "

static void check_literals(void)
{
    check_body("0", PUSH "bb 00 00 00 00");
    check_body("42", PUSH "bb 2a 00 00 00");
    check_body("$ff", PUSH "bb ff 00 00 00");
    check_body("0xFFFFFFFF", PUSH "bb ff ff ff ff");
    check_body("-1", PUSH "48 c7 c3 ff ff ff ff");
    check_body("-2147483648", PUSH "48 c7 c3 00 00 00 80");
    check_body("-2147483649", PUSH "48 bb ff ff ff 7f ff ff ff ff");
    check_body("4294967296", PUSH "48 bb 00 00 00 00 01 00 00 00");
    check_body("9223372036854775807", PUSH "48 bb ff ff ff ff ff ff ff 7f");
    check_body("-9223372036854775808", PUSH "48 bb 00 00 00 00 00 00 00 80");
    check_body("$FFFFFFFFFFFFFFFF", PUSH "48 c7 c3 ff ff ff ff");
    check_body("-$10", PUSH "48 c7 c3 f0 ff ff ff");
}

/* Expected definition bodies. "call:NAME" stands for `call rel32` to the
 * named primitive, whose displacement depends on where the body lands. */
static void check_def(const char *src, const char *name, const char *expect)
{
    forth_ctx ctx;
    uint8_t want[256];
    size_t n = 0;
    const uint8_t *img = compile_ok(src, &ctx);
    const forth_def *def;
    const char *p = expect;

    if (img == NULL) {
        return;
    }
    def = forth_find_def(&ctx, name);
    CHECK(def != NULL, "definition '%s' not recorded", name);
    if (def == NULL) {
        return;
    }
    while (*p != '\0') {
        if (*p == ' ') {
            p++;
        } else if (strncmp(p, "call:", 5) == 0) {
            char prim[16];
            size_t k = 0;
            p += 5;
            while (*p != '\0' && *p != ' ' && k < sizeof prim - 1) {
                prim[k++] = *p++;
            }
            prim[k] = '\0';
            long target = forth_primitive_offset(prim);
            int32_t rel = (int32_t)(target - (long)(def->start + n + 5));
            want[n++] = 0xE8;
            for (int b = 0; b < 4; b++) {
                want[n++] = (uint8_t)((uint32_t)rel >> (8 * b));
            }
        } else {
            char *end;
            want[n++] = (uint8_t)strtoul(p, &end, 16);
            p = end;
        }
    }
    CHECK(def->end - def->start == n &&
              memcmp(img + def->start, want, n) == 0,
          "definition in \"%s\" differs from golden", src);
    /* Top-level code jumps over the definition. */
    CHECK(img[def->start - 5] == 0xE9 &&
              rel32_at(img + def->start - 4) == (int32_t)(def->end - def->start),
          "no jump over definition in \"%s\"", src);
}

#define ZBRANCH "48 85 db 48 8b 5d 00 48 8d 6d 08 0f 84 "

static void check_control_shapes(void)
{
    check_def(": t ;", "t", "c3");
    check_def(": t if 1 then ;", "t",
              ZBRANCH "0d 00 00 00 " PUSH "bb 01 00 00 00 c3");
    check_def(": t if 1 else 2 then ;", "t",
              ZBRANCH "12 00 00 00 " PUSH "bb 01 00 00 00 "
              "e9 0d 00 00 00 " PUSH "bb 02 00 00 00 c3");
    check_def(": t begin 1- dup 0= until ;", "t",
              "call:1- call:dup call:0= " ZBRANCH "e0 ff ff ff c3");
    check_def(": t begin dup while 1- repeat ;", "t",
              "call:dup " ZBRANCH "0a 00 00 00 call:1- e9 e0 ff ff ff c3");
    check_def(": t do i drop loop ;", "t",
              "call:(do) call:i call:drop call:(loop) 0f 85 eb ff ff ff "
              "4d 8d 64 24 10 c3");
    check_def(": t if begin dup while 1- repeat then ;", "t",
              ZBRANCH "20 00 00 00 call:dup " ZBRANCH "0a 00 00 00 "
              "call:1- e9 e0 ff ff ff c3");
    check_def(": t if if 1 then else 2 then ;", "t",
              ZBRANCH "23 00 00 00 " ZBRANCH "0d 00 00 00 "
              PUSH "bb 01 00 00 00 e9 0d 00 00 00 " PUSH "bb 02 00 00 00 c3");
    /* A call to an earlier colon definition targets its first byte. */
    {
        forth_ctx ctx;
        const uint8_t *img = compile_ok(": a ; : b a ;", &ctx);
        if (img != NULL) {
            const forth_def *a = forth_find_def(&ctx, "a");
            const forth_def *b = forth_find_def(&ctx, "b");
            CHECK(a && b && img[b->start] == 0xE8 &&
                      (long)b->start + 5 + rel32_at(img + b->start + 1) ==
                          (long)a->start,
                  "call to colon definition has wrong target");
        }
    }
}

/* ---- Whole-image golden files ------------------------------------------ */

static size_t read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f == NULL) {
        return (size_t)-1;
    }
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

static void check_golden_file(const char *dir, const char *stem)
{
    static char src[16384], hex[65536];
    static uint8_t want[TEST_IMAGE_MAX];
    char path[512];
    forth_ctx ctx;

    snprintf(path, sizeof path, "%s/golden/%s.fs", dir, stem);
    CHECK(read_file(path, src, sizeof src) != (size_t)-1, "cannot read %s",
          path);
    snprintf(path, sizeof path, "%s/golden/%s.golden", dir, stem);
    CHECK(read_file(path, hex, sizeof hex) != (size_t)-1, "cannot read %s",
          path);
    const uint8_t *img = compile_ok(src, &ctx);
    if (img == NULL) {
        return;
    }
    size_t n = parse_hex(hex, want, sizeof want);
    CHECK(n == ctx.code_len && memcmp(img, want, n) == 0,
          "%s does not match the compiler output (run `make golden` only "
          "if the change is intended)", path);
}

void test_golden(const char *dir)
{
    check_primitive_table();
    check_library_layout();
    check_primitive_calls();
    check_literals();
    check_control_shapes();
    check_golden_file(dir, "answer");
    check_golden_file(dir, "shapes");
}
