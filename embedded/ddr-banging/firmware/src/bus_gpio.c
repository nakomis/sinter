#include "bus_gpio.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "pins.h"

#define CA_PINS_MASK  ((uint64_t)((1u << CA_BITS) - 1) << PIN_CA_BASE)
#define CK            (1ull << (PIN_CA_BASE + CA_CK))
#define CKN           (1ull << (PIN_CA_BASE + CA_CKN))
#define DQS           (1ull << PIN_DQS)
#define DQSN          (1ull << PIN_DQSN)
#define CA_ONLY       ((uint64_t)CA_MASK << PIN_CA_BASE)

static uint32_t quarter_cycles;   // busy-wait per quarter clock
static uint32_t tck_ns = 1000;    // measured period
static bool driving;

#define LANE_MASK (7ull << PIN_LANE0)   // decoder A/B/C: held at lane 0

static const uint64_t outputs = CA_PINS_MASK | DIR_MASK | LANE_MASK | (1ull << PIN_RESETN);

void bus_init(void)
{
    for (int g = 0; g <= 40; g++) {
        gpio_init(g);
        gpio_disable_pulls(g);
    }
    // idle: CK low, CK# high, CKE low, both CS# high, RAS#/CAS#/WE# high
    uint64_t idle = CKN | ((uint64_t)ddr3_encode(DDR3_DES, 0, 0, false) << PIN_CA_BASE);
    // RESET# low, DIR low (B->A); DQS# output latch high so the first drive
    // starts from DQS low / DQS# high
    gpio_put_masked64(outputs | DQ_MASK | DQS_MASK, idle | DQSN);
    gpio_set_dir_masked64(outputs | DQ_MASK | DQS_MASK, outputs);
    driving = false;
    bus_set_tck(1000);
}

void bus_reset(bool asserted)
{
    gpio_put(PIN_RESETN, !asserted);
}

uint32_t bus_tck(void)
{
    return tck_ns;
}

// RP2350 -> DRAM: DIR high first so the translator's A side is an input
// before the GPIOs drive it. DRAM -> RP2350: GPIOs off first, then DIR low.
static inline void drive(bool on)
{
    if (on == driving) return;
    if (on) {
        gpio_put_masked64(DIR_MASK, DIR_MASK);
        gpio_set_dir_out_masked64(DQ_MASK | DQS_MASK);
    } else {
        gpio_set_dir_in_masked64(DQ_MASK | DQS_MASK);
        gpio_put_masked64(DIR_MASK, 0);
    }
    driving = on;
}

static inline void wait_q(void)
{
    busy_wait_at_least_cycles(quarter_cycles);
}

// Each CK cycle is four quarters:
//   t0  CK falls (end of previous cycle); command/address for this cycle
//   t1  write data for the rising DQS edge; read beat 1 of the previous cycle
//   t2  CK rises (DRAM registers the command); DQS rises on strobe cycles
//   t3  write data for the falling DQS edge; read beat 0 of this cycle
// so write data is centred on each DQS edge and reads sample mid-beat.
uint32_t __not_in_flash_func(bus_run)(const ddr3_seq_t *s, bus_sample_t *samples,
                                      unsigned max_samples)
{
    unsigned n = 0;
    bool pending = false;   // previous cycle's beat 1 still to sample
    uint32_t irq = save_and_disable_interrupts();
    uint64_t t0 = time_us_64();
    drive(false);
    gpio_put_masked64(DQ_MASK | DQS_MASK, DQSN);   // output latches: the first drive starts clean

    for (size_t i = 0; i < s->len; i++) {
        const ddr3_cycle_t *c = &s->cycles[i];
        bool dr = c->flags & CY_DRIVE;
        uint64_t strobe_m = dr ? DQS_MASK : 0;

        // t0: CK low half. DQS low / DQS# high whenever driven (incl. preamble)
        drive(dr);
        gpio_put_masked64(CA_ONLY | CK | CKN | strobe_m,
                          (uint64_t)c->ca << PIN_CA_BASE | CKN | (dr ? DQSN : 0));
        wait_q();
        // t1
        if (pending) {   // only ever set after a sample was stored
            samples[n - 1].beat[1] = gpio_get_all64() & 0x3FF;
            pending = false;
        }
        if (dr) gpio_put_masked64(DQ_MASK, (uint64_t)c->dq[0] << PIN_DQ0);
        wait_q();
        // t2: CK high half. DQS and DQS# always complementary
        uint64_t strobe = (c->flags & CY_STROBE) ? DQS : DQSN;
        gpio_put_masked64(CK | CKN | strobe_m, CK | (dr ? strobe : 0));
        wait_q();
        // t3
        if (dr) gpio_put_masked64(DQ_MASK, (uint64_t)c->dq[1] << PIN_DQ0);
        if ((c->flags & CY_SAMPLE) && samples && n < max_samples) {
            samples[n++].beat[0] = gpio_get_all64() & 0x3FF;
            pending = true;
        }
        wait_q();
    }
    // final falling edge
    gpio_put_masked64(CK | CKN | (driving ? DQS_MASK : 0), CKN | (driving ? DQSN : 0));
    if (pending) {
        wait_q();
        samples[n - 1].beat[1] = gpio_get_all64() & 0x3FF;
    }
    drive(false);

    uint32_t us = (uint32_t)(time_us_64() - t0);
    restore_interrupts(irq);
    return us;
}

uint32_t bus_set_tck(uint32_t target_ns)
{
    // Start from the nominal busy-wait, measure a run of deselect cycles with
    // CKE low (the DRAM ignores them), then correct for loop overhead once.
    static ddr3_cycle_t cal[2000];
    ddr3_seq_t s;
    ddr3_seq_init(&s, cal, 2000, target_ns);
    for (int i = 0; i < 2000; i++) ddr3_seq_cmd(&s, DDR3_DES, 0, 0);

    uint32_t hz = clock_get_hz(clk_sys);
    uint32_t nominal = (uint32_t)((uint64_t)hz * target_ns / 4000000000ull);
    quarter_cycles = nominal;
    uint32_t period = bus_run(&s, NULL, 0) * 1000u / 2000u;
    if (period > target_ns) {
        uint32_t over = (uint32_t)((uint64_t)hz * (period - target_ns) / 4000000000ull);
        quarter_cycles = over < nominal ? nominal - over : 0;
        period = bus_run(&s, NULL, 0) * 1000u / 2000u;
    }
    tck_ns = period;
    return period;
}

void bus_hold(int gpio)
{
    drive(false);
    uint64_t idle = CKN | ((uint64_t)ddr3_encode(DDR3_DES, 0, 0, false) << PIN_CA_BASE)
                  | (1ull << PIN_RESETN);
    if (gpio < 0) {
        gpio_put_masked64(outputs, idle);
        return;
    }
    uint64_t bit = 1ull << gpio;
    if (bit & (DQ_MASK | DQS_MASK | DIR_MASK)) {
        // through the translator: drive the A side (low) before turning DIR on
        drive(true);
        uint64_t dir = (bit & DIR_MASK) ? bit : DIR_MASK;
        gpio_put_masked64(outputs | DQ_MASK | DQS_MASK, dir | (bit & ~DIR_MASK));
    } else {
        gpio_put_masked64(outputs, bit);
    }
}
