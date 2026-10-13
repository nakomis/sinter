/*
 * main.c — runs every suite. Usage: forth_tests [TESTDIR]
 * TESTDIR (default "tests") holds the golden directory of .fs sources and
 * their .golden images.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"

int check_count;
int check_failures;
int check_skips;

void check_skip(const char *suite, const char *why)
{
    check_skips++;
    printf("SKIP %s: %s\n", suite, why);
}

const uint8_t *compile_ok(const char *src, forth_ctx *ctx)
{
    static uint8_t image[TEST_IMAGE_MAX];
    forth_status st = forth_compile(ctx, src, strlen(src), image, sizeof image);
    CHECK(st == FORTH_OK, "compile failed for \"%s\": %s", src,
          ctx->error.message);
    return st == FORTH_OK ? image : NULL;
}

size_t parse_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*hex != '\0' && n < cap) {
        char *end;
        unsigned long v = strtoul(hex, &end, 16);
        if (end == hex) {
            hex++;
            continue;
        }
        out[n++] = (uint8_t)v;
        hex = end;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests";

#define RUN(label, call)                                                   \
    do {                                                                   \
        int count0 = check_count, fail0 = check_failures;                  \
        call;                                                              \
        printf("%-8s %5d checks, %d failed\n", label, check_count - count0, \
               check_failures - fail0);                                    \
    } while (0)

    RUN("golden", test_golden(dir));
    RUN("errors", test_errors());
    RUN("disasm", test_disasm());
    RUN("exec", test_exec());
#undef RUN

    printf("\n%d checks, %d failed, %d suites skipped\n", check_count,
           check_failures, check_skips);
    return check_failures == 0 ? 0 : 1;
}
