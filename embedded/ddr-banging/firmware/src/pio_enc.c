#include "pio_enc.h"

#define CA_PIN_MASK ((1u << PIOENC_CA_BITS) - 1)   // C/A bits 2..24 shifted down

ddr3_cycle_t pioenc_normalise(ddr3_cycle_t c)
{
    c.ca &= ~((1u << CA_CK) | (1u << CA_CKN));
    c.flags &= CY_DRIVE | CY_STROBE | CY_SAMPLE;
    if (!(c.flags & CY_DRIVE)) c.dq[0] = c.dq[1] = 0;
    return c;
}

static bool same(ddr3_cycle_t a, ddr3_cycle_t b)
{
    return a.ca == b.ca && a.flags == b.flags && a.dq[0] == b.dq[0] && a.dq[1] == b.dq[1];
}

// Cycle i as encoded: normalised, plus whether cycle i-1 sampled
static ddr3_cycle_t at(const ddr3_cycle_t *c, size_t i)
{
    ddr3_cycle_t x = pioenc_normalise(c[i]);
    if (i && (c[i - 1].flags & CY_SAMPLE)) x.flags |= PIOENC_PREV_SAMPLE;
    return x;
}

size_t pioenc_encode(const ddr3_cycle_t *c, size_t n, uint32_t *cmd, uint32_t *data,
                     size_t cap, bool *overflow)
{
    size_t w = 0;
    *overflow = false;
    for (size_t i = 0; i < n;) {
        ddr3_cycle_t x = at(c, i);
        uint32_t rep = 0;
        while (i + 1 + rep < n && rep < PIOENC_REP_MAX && same(x, at(c, i + 1 + rep)))
            rep++;
        if (w == cap) {
            *overflow = true;
            return w;
        }
        cmd[w] = (x.ca >> CA_CKE) & CA_PIN_MASK;
        data[w] = (uint32_t)x.flags | (uint32_t)x.dq[0] << 4 | (uint32_t)x.dq[1] << 12;
        cmd[w] |= rep << PIOENC_REP_SHIFT;
        data[w] |= rep << PIOENC_REP_SHIFT;
        w++;
        i += 1 + rep;
    }
    return w;
}

size_t pioenc_decode(const uint32_t *cmd, const uint32_t *data, size_t words,
                     ddr3_cycle_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t w = 0; w < words; w++) {
        uint32_t rep = cmd[w] >> PIOENC_REP_SHIFT;
        if (rep != data[w] >> PIOENC_REP_SHIFT) return 0;   // the SMs would drift apart
        ddr3_cycle_t c = {
            .ca = (cmd[w] & CA_PIN_MASK) << CA_CKE,
            .flags = (uint8_t)(data[w] & 7u),   // PREV_SAMPLE is derived; dropped
            .dq = { (uint8_t)(data[w] >> 4), (uint8_t)(data[w] >> 12) },
        };
        for (uint32_t k = 0; k <= rep; k++) {
            if (n == cap) return 0;
            out[n++] = c;
        }
    }
    return n;
}

bool pioenc_timing(uint32_t tck_ns, uint32_t sysclk_hz, unsigned instr, unsigned max_delay,
                   pioenc_timing_t *out)
{
    // system clocks per CK, in thousandths, so rounding happens once
    uint64_t want_milli = (uint64_t)tck_ns * sysclk_hz / 1000000u;   // = tck * f / 1e9 * 1000
    bool found = false;
    uint64_t best_err = UINT64_MAX;
    for (unsigned d = 0; d <= max_delay; d++) {
        uint64_t per_div = 4ull * instr * (d + 1);   // system clocks per CK when div = 1
        uint64_t div = (want_milli + per_div * 500) / (per_div * 1000);   // nearest
        if (div < 1) div = 1;
        if (div > 65535) continue;
        uint64_t got_milli = div * per_div * 1000;
        uint64_t err = got_milli > want_milli ? got_milli - want_milli : want_milli - got_milli;
        // ties go to the larger delay: fewer divider steps, same timing
        if (err <= best_err) {
            best_err = err;
            out->div = (uint32_t)div;
            out->delay = d;
            out->tck_ps = (uint32_t)(got_milli * 1000000000ull / sysclk_hz);
            found = true;
        }
    }
    // too slow even flat out: the fastest it can do is more than 10% over
    return found && (uint64_t)out->tck_ps <= (uint64_t)tck_ns * 1100;
}
