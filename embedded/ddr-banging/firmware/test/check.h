// Minimal host test harness: CHECK records failures, TEST registers nothing
// (tests are called from main in run_tests.c).
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

extern int check_failures;

#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); check_failures++; } } while (0)

#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("  FAIL %s:%d: %s == %s (%lld vs %lld, 0x%llx vs 0x%llx)\n", \
        __FILE__, __LINE__, #a, #b, _a, _b, _a, _b); check_failures++; } } while (0)

#define RUN(fn) do { printf("%s\n", #fn); fn(); } while (0)

#endif
