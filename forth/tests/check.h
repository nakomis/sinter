/*
 * check.h — a deliberately tiny test framework shared by the suites.
 */
#ifndef SINTER_FORTH_CHECK_H
#define SINTER_FORTH_CHECK_H

#include <stdio.h>

#include "internal.h"

extern int check_count;
extern int check_failures;
extern int check_skips;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        check_count++;                                                     \
        if (!(cond)) {                                                     \
            check_failures++;                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

void check_skip(const char *suite, const char *why);

/* Compile `src` into a static buffer shared by all tests (overwritten on
 * each call). Records a failure and returns NULL if compilation fails. */
#define TEST_IMAGE_MAX 65536
const uint8_t *compile_ok(const char *src, forth_ctx *ctx);

/* Expected primitive encodings, shared by the golden and objdump suites. */
typedef struct prim_golden {
    const char *name;
    const char *hex;       /* space-separated bytes */
    const char *disasm;    /* GNU objdump -M intel, one insn per '|' */
} prim_golden;
extern const prim_golden prim_goldens[];
extern const size_t prim_golden_count;

/* Parse space-separated hex into out; returns the byte count. */
size_t parse_hex(const char *hex, uint8_t *out, size_t cap);

void test_golden(const char *dir);
void test_errors(void);
void test_disasm(void);
void test_exec(void);

#endif
