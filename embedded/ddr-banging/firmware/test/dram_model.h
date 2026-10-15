// Behavioural DDR3 data model on the simulated pins (see dram_model.c).
#ifndef DRAM_MODEL_H
#define DRAM_MODEL_H

#include <stdbool.h>
#include <stdint.h>

void model_reset(void);
void model_rewind(void);   // sim_log restarted: read it from the start, keep the memory
void model_set_ticks_per_ck(long ticks);   // 4 (bus_run, the default) or system clocks per CK (PIO)
uint64_t model_input(long t);        // install as sim_input
uint8_t model_byte(uint32_t addr);   // what the "DRAM" holds
bool model_out_of_range(void);       // a write went past the modelled 1 MB

#endif
