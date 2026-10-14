/*
 * test_disasm.c — the disassembler is a higher authority on what we
 * emitted than our own emitter is (docs/notes/verify-against-silicon.md).
 *
 * Runs `objdump -D -b binary -m i386:x86-64 -M intel` over each primitive
 * and over whole images, and checks that:
 *   - every primitive disassembles to exactly the expected instructions;
 *   - whole images decode cleanly, with an instruction boundary at the
 *     entry point, every primitive, main and every colon definition, and
 *     nothing but ud2 in the guard tail.
 *
 * Needs GNU objdump (set OBJDUMP to override, e.g. OBJDUMP=gobjdump on
 * macOS with Homebrew binutils); skipped if it is not available.
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"

#define MAX_INSNS 4096

typedef struct insn {
    unsigned long addr;
    char text[96];        /* mnemonic and operands, whitespace collapsed */
} insn;

static const char *objdump_cmd(void)
{
    const char *env = getenv("OBJDUMP");
    return env != NULL && env[0] != '\0' ? env : "objdump";
}

static int have_gnu_objdump(void)
{
    char cmd[256], line[256];
    int gnu = 0;
    FILE *p;
    snprintf(cmd, sizeof cmd, "%s --version 2>/dev/null", objdump_cmd());
    p = popen(cmd, "r");
    if (p == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, p) != NULL) {
        gnu |= strstr(line, "GNU objdump") != NULL;
    }
    pclose(p);
    return gnu;
}

/* Disassemble `len` bytes; returns the number of instructions, or -1. */
static int disassemble(const uint8_t *code, size_t len, insn *out, int max)
{
    char path[] = "/tmp/sinter-forth-XXXXXX";
    char cmd[512], line[512];
    int fd = mkstemp(path), n = 0;
    FILE *p;

    if (fd < 0) {
        return -1;
    }
    if (write(fd, code, len) != (ssize_t)len) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    snprintf(cmd, sizeof cmd,
             "%s -D -b binary -m i386:x86-64 -M intel %s 2>&1", objdump_cmd(),
             path);
    p = popen(cmd, "r");
    if (p == NULL) {
        unlink(path);
        return -1;
    }
    /* Instruction lines look like "  1f:\t48 8b 5d 08    \tmov    rbx,..."
     * and continuation lines (long encodings) have no second tab. */
    while (fgets(line, sizeof line, p) != NULL && n < max) {
        char *colon = strchr(line, ':'), *tab1, *tab2, *end;
        unsigned long addr = strtoul(line, &end, 16);
        if (colon == NULL || end != colon || colon[1] != '\t') {
            continue;
        }
        tab1 = colon + 1;
        tab2 = strchr(tab1 + 1, '\t');
        if (tab2 == NULL) {
            continue;
        }
        size_t k = 0;
        int space = 0;
        for (const char *s = tab2 + 1; *s != '\0' && *s != '\n'; s++) {
            if (*s == ' ' || *s == '\t') {
                space = k > 0;
                continue;
            }
            if (space && k < sizeof out[n].text - 1) {
                out[n].text[k++] = ' ';
            }
            space = 0;
            if (k < sizeof out[n].text - 1) {
                out[n].text[k++] = *s;
            }
        }
        out[n].text[k] = '\0';
        out[n].addr = addr;
        n++;
    }
    pclose(p);
    unlink(path);
    return n;
}

static void check_primitives(void)
{
    static insn insns[64];

    for (size_t i = 0; i < prim_golden_count; i++) {
        const prim_golden *g = &prim_goldens[i];
        uint8_t bytes[64];
        size_t len = parse_hex(g->hex, bytes, sizeof bytes);
        int n = disassemble(bytes, len, insns, 64);
        const char *want = g->disasm;
        int k = 0;

        CHECK(n > 0, "objdump failed on '%s'", g->name);
        while (*want != '\0' && k < n) {
            size_t wl = strcspn(want, "|");
            CHECK(strlen(insns[k].text) == wl &&
                      strncmp(insns[k].text, want, wl) == 0,
                  "'%s' insn %d: objdump says \"%s\", expected \"%.*s\"",
                  g->name, k, insns[k].text, (int)wl, want);
            want += wl + (want[wl] == '|');
            k++;
        }
        CHECK(*want == '\0' && k == n, "'%s': %d insns, expected count "
              "differs", g->name, n);
    }
}

static int has_boundary(const insn *insns, int n, unsigned long addr)
{
    for (int i = 0; i < n; i++) {
        if (insns[i].addr == addr) {
            return 1;
        }
    }
    return 0;
}

static void check_image(const char *src)
{
    static insn insns[MAX_INSNS];
    forth_ctx ctx;
    const uint8_t *img = compile_ok(src, &ctx);
    int n;

    if (img == NULL) {
        return;
    }
    n = disassemble(img, ctx.code_len, insns, MAX_INSNS);
    CHECK(n > 0, "objdump failed on image of \"%.40s\"", src);
    for (int i = 0; i < n; i++) {
        CHECK(strstr(insns[i].text, "(bad)") == NULL,
              "undecodable bytes at 0x%lx in \"%.40s\"", insns[i].addr, src);
        if (insns[i].addr >= ctx.code_len - FORTH_GUARD_BYTES) {
            CHECK(strcmp(insns[i].text, "ud2") == 0,
                  "guard tail has \"%s\" at 0x%lx", insns[i].text,
                  insns[i].addr);
        }
    }
    CHECK(has_boundary(insns, n, 0) &&
              has_boundary(insns, n, ctx.main_offset),
          "entry or main is not an instruction boundary");
    for (size_t i = 0; i < forth_word_count; i++) {
        long off = forth_word_offset(&forth_words[i]);
        if (off >= 0) {
            CHECK(has_boundary(insns, n, (unsigned long)off),
                  "'%s' at 0x%lx is not an instruction boundary",
                  forth_words[i].name, off);
        }
    }
    for (size_t i = 0; i < ctx.ndefs; i++) {
        CHECK(has_boundary(insns, n, ctx.defs[i].start),
              "definition '%s' is not an instruction boundary",
              ctx.defs[i].name);
    }
    CHECK(strcmp(insns[n - 1].text, "ud2") == 0 &&
              insns[n - 1].addr == ctx.code_len - 2,
          "image does not end with the ud2 guard");
}

void test_disasm(void)
{
    if (!have_gnu_objdump()) {
        check_skip("disasm", "GNU objdump not found (set OBJDUMP)");
        return;
    }
    check_primitives();
    check_image("");
    check_image(": answer 6 7 * result! ; answer");
    check_image(": sign dup 0< if drop -1 else 0= if 0 else 1 then then ;\n"
                ": gcd begin dup while swap over mod repeat drop ;\n"
                ": sum 0 swap 0 do i + loop ;\n"
                "-5 sign 1071 462 gcd 101 sum + + 1 -1 4294967296 "
                "-9223372036854775808 result!");
}
