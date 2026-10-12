// Simulated GPIO block for host tests of bus_gpio.c. Time advances one unit
// per busy-wait, i.e. one quarter clock in bus_run.
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { long t; uint64_t out, oe; } sim_snap_t;

#define SIM_MAX 200000
extern sim_snap_t sim_log[SIM_MAX];
extern size_t sim_len;
extern long sim_t;
extern uint64_t sim_out, sim_oe;
extern uint64_t (*sim_input)(long t);   // what the DRAM side drives

void sim_reset(void);

#endif
