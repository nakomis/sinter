/*
 * test_errors.c — every compile error is reported with the right status,
 * position and a message the LLM can act on.
 */
#include <string.h>

#include "check.h"

static void expect_error(const char *src, forth_status status, unsigned line,
                         unsigned column, const char *fragment)
{
    static uint8_t image[TEST_IMAGE_MAX];
    forth_ctx ctx;
    forth_status st = forth_compile(&ctx, src, strlen(src), image,
                                    sizeof image);
    CHECK(st == status, "\"%s\": got %s, want %s", src, forth_status_name(st),
          forth_status_name(status));
    CHECK(ctx.error.status == status, "\"%s\": error.status not set", src);
    CHECK(ctx.error.line == line && ctx.error.column == column,
          "\"%s\": error at %u:%u, want %u:%u", src, ctx.error.line,
          ctx.error.column, line, column);
    CHECK(strstr(ctx.error.message, fragment) != NULL,
          "\"%s\": message \"%s\" lacks \"%s\"", src, ctx.error.message,
          fragment);
}

void test_errors(void)
{
    expect_error("1 2 frob", FORTH_ERR_UNKNOWN_WORD, 1, 5,
                 "line 1, column 5: unknown word 'frob'");
    expect_error(": sq dup * ;\n\n  3 squared", FORTH_ERR_UNKNOWN_WORD, 3, 5,
                 "unknown word 'squared'");
    /* Hidden helpers are not reachable from source. */
    expect_error("(do)", FORTH_ERR_UNKNOWN_WORD, 1, 1, "'(do)'");
    expect_error("1 2 3.5", FORTH_ERR_UNKNOWN_WORD, 1, 5, "'3.5'");
    expect_error("0x", FORTH_ERR_UNKNOWN_WORD, 1, 1, "'0x'");
    /* A definition is not visible inside itself (no recursion yet). */
    expect_error(": f f ;", FORTH_ERR_UNKNOWN_WORD, 1, 5, "'f'");

    expect_error("9223372036854775808", FORTH_ERR_NUMBER_RANGE, 1, 1,
                 "does not fit");
    expect_error("-9223372036854775809", FORTH_ERR_NUMBER_RANGE, 1, 1,
                 "does not fit");
    expect_error("$10000000000000000", FORTH_ERR_NUMBER_RANGE, 1, 1,
                 "does not fit");

    expect_error(": f then ;", FORTH_ERR_CONTROL_MISMATCH, 1, 5,
                 "'then' without a matching 'if'");
    expect_error(": f else ;", FORTH_ERR_CONTROL_MISMATCH, 1, 5,
                 "'else' without a matching 'if'");
    expect_error(": f until ;", FORTH_ERR_CONTROL_MISMATCH, 1, 5,
                 "'until' without a matching 'begin'");
    expect_error(": f while ;", FORTH_ERR_CONTROL_MISMATCH, 1, 5,
                 "'while' without a matching 'begin'");
    expect_error(": f begin repeat ;", FORTH_ERR_CONTROL_MISMATCH, 1, 11,
                 "'repeat' without a matching 'while'");
    expect_error(": f loop ;", FORTH_ERR_CONTROL_MISMATCH, 1, 5,
                 "'loop' without a matching 'do'");
    expect_error(": f 1 if begin then ;", FORTH_ERR_CONTROL_MISMATCH, 1, 16,
                 "'then' without a matching 'if'");
    expect_error(": f do if loop ;", FORTH_ERR_CONTROL_MISMATCH, 1, 11,
                 "'loop' without a matching 'do'");

    expect_error(": f 1 if 2 ;", FORTH_ERR_CONTROL_UNCLOSED, 1, 12,
                 "'if' at line 1, column 7 is not closed before ';'");
    expect_error(": f\n  begin 1 ;", FORTH_ERR_CONTROL_UNCLOSED, 2, 11,
                 "'begin' at line 2, column 3 is not closed");
    expect_error("10 0 do i", FORTH_ERR_CONTROL_UNCLOSED, 1, 10,
                 "'do' at line 1, column 6 is not closed before the end");
    expect_error(": f begin 1 while", FORTH_ERR_CONTROL_UNCLOSED, 1, 18,
                 "'while' at line 1, column 13");

    expect_error(": f : g ;", FORTH_ERR_NESTED_DEFINITION, 1, 5,
                 "inside the definition of 'f'");
    expect_error("1 if : g ; then", FORTH_ERR_NESTED_DEFINITION, 1, 6,
                 "open control structure");
    expect_error("1 2 ;", FORTH_ERR_STRAY_SEMICOLON, 1, 5,
                 "';' outside a colon definition");
    expect_error(": f 1 2 +", FORTH_ERR_UNTERMINATED_DEF, 1, 10,
                 "definition of 'f' is missing its ';'");
    expect_error("1 :", FORTH_ERR_MISSING_NAME, 1, 3,
                 "':' must be followed by a name");
    expect_error(": abcdefghijabcdefghijabcdefghijab ;", FORTH_ERR_BAD_NAME,
                 1, 3, "longer than 31");
    expect_error(": IF 1 ;", FORTH_ERR_BAD_NAME, 1, 3,
                 "'if' is a reserved word");
    expect_error(": ; ;", FORTH_ERR_BAD_NAME, 1, 3, "reserved");
    expect_error("1 ( unterminated", FORTH_ERR_UNTERMINATED_COMMENT, 1, 3,
                 "never closed");

    {
        static char src[FORTH_MAX_DEFS * 10 + 16];
        size_t n = 0, last = 0;
        for (int i = 0; i <= FORTH_MAX_DEFS; i++) {
            last = n;
            n += (size_t)snprintf(src + n, sizeof src - n, ": w%d ; ", i);
        }
        /* Reported at the name of the definition that does not fit. */
        expect_error(src, FORTH_ERR_TOO_MANY_DEFS, 1, (unsigned)last + 3,
                     "more than 128");
    }
    {
        static char src[FORTH_MAX_CONTROL * 6 + 16];
        size_t n = 0;
        for (int i = 0; i <= FORTH_MAX_CONTROL; i++) {
            n += (size_t)snprintf(src + n, sizeof src - n, "begin ");
        }
        expect_error(src, FORTH_ERR_CONTROL_DEPTH, 1, 1 + 6 * 32,
                     "nested more than 32 deep");
    }

    /* Output buffer too small: every prefix length must fail cleanly. */
    {
        static uint8_t image[TEST_IMAGE_MAX];
        const char *src = ": sq dup * ; 10 0 do i sq drop loop 7 result!";
        forth_ctx ctx;
        size_t need;
        CHECK(forth_compile(&ctx, src, strlen(src), image, sizeof image) ==
                  FORTH_OK, "baseline compile failed");
        need = ctx.code_len;
        for (size_t cap = 0; cap < need; cap++) {
            forth_status st = forth_compile(&ctx, src, strlen(src), image, cap);
            CHECK(st == FORTH_ERR_OUTPUT_FULL, "cap %zu: got %s", cap,
                  forth_status_name(st));
        }
        CHECK(forth_compile(&ctx, src, strlen(src), image, need) == FORTH_OK,
              "exact-size buffer rejected");
    }

    /* API misuse. */
    {
        uint8_t image[16];
        forth_ctx ctx;
        CHECK(forth_compile(NULL, "1", 1, image, sizeof image) ==
                  FORTH_ERR_ARGUMENT, "NULL ctx accepted");
        CHECK(forth_compile(&ctx, NULL, 1, image, sizeof image) ==
                  FORTH_ERR_ARGUMENT, "NULL source accepted");
        CHECK(forth_compile(&ctx, "1", 1, NULL, 0) == FORTH_ERR_ARGUMENT,
              "NULL buffer accepted");
    }

    /* Comments, case-insensitivity and non-terminated input are fine. */
    {
        forth_ctx ctx;
        uint8_t image[4096];
        const char src[] = "( a comment ) 1 \\ to end of line\n DUP Drop";
        CHECK(forth_compile(&ctx, src, sizeof src - 1, image, sizeof image) ==
                  FORTH_OK, "comment/case program rejected: %s",
              ctx.error.message);
        CHECK(forth_compile(&ctx, "1 2 +xyz", 5, image, sizeof image) ==
                  FORTH_OK, "length-delimited source not honoured: %s",
              ctx.error.message);
    }

    /* Status names are distinct and non-empty. */
    for (int a = FORTH_OK; a <= FORTH_ERR_OUTPUT_FULL; a++) {
        CHECK(strcmp(forth_status_name((forth_status)a), "unknown-status") != 0,
              "status %d has no name", a);
        for (int b = a + 1; b <= FORTH_ERR_OUTPUT_FULL; b++) {
            CHECK(strcmp(forth_status_name((forth_status)a),
                         forth_status_name((forth_status)b)) != 0,
                  "statuses %d and %d share a name", a, b);
        }
    }
}
