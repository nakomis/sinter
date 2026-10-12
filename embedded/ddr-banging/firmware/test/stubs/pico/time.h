#ifndef STUB_TIME_H
#define STUB_TIME_H
#include <stdint.h>
// one simulated quarter clock = 250 ns at the default 1 MHz
#include "../sim.h"
static inline uint64_t time_us_64(void) { return (uint64_t)sim_t / 4; }
#endif
