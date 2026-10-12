#include "sim.h"

sim_snap_t sim_log[SIM_MAX];
size_t sim_len;
long sim_t;
uint64_t sim_out, sim_oe;
uint64_t (*sim_input)(long t);

void sim_reset(void)
{
    sim_len = 0;
    sim_t = 0;
    sim_input = 0;
}

void sim_record(void)
{
    if (sim_len < SIM_MAX) sim_log[sim_len++] = (sim_snap_t){ sim_t, sim_out, sim_oe };
}
