#ifndef STUB_CLOCKS_H
#define STUB_CLOCKS_H
#include <stdint.h>
#define clk_sys 0
static inline uint32_t clock_get_hz(int c) { (void)c; return 150000000u; }
#endif
