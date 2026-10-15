#ifndef STUB_GPIO_H
#define STUB_GPIO_H
#include "../sim.h"

void sim_record(void);
#define __not_in_flash_func(f) f

static inline void gpio_init(unsigned g) { (void)g; }
static inline void gpio_disable_pulls(unsigned g) { (void)g; }
static inline void gpio_put_masked64(uint64_t m, uint64_t v) { sim_out = (sim_out & ~m) | (v & m); sim_record(); }
static inline void gpio_put(unsigned g, bool v) { gpio_put_masked64(1ull << g, (uint64_t)v << g); }
static inline void gpio_set_dir_masked64(uint64_t m, uint64_t v) { sim_oe = (sim_oe & ~m) | (v & m); sim_record(); }
static inline void gpio_set_dir_out_masked64(uint64_t m) { sim_oe |= m; sim_record(); }
static inline void gpio_set_dir_in_masked64(uint64_t m) { sim_oe &= ~m; sim_record(); }
static inline uint64_t gpio_get_all64(void)
{
    uint64_t in = sim_input ? sim_input(sim_t) : 0;
    return (sim_out & sim_oe) | (in & ~sim_oe);
}
static inline void busy_wait_at_least_cycles(uint32_t c) { (void)c; sim_t++; }
#endif
