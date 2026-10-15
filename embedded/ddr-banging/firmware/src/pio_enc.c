#include "pio_enc.h"

#define CA_PIN_MASK ((1u << PIOENC_CA_BITS) - 1)   // C/A bits 2..24 shifted down

ddr3_cycle_t pioenc_normalise(ddr3_cycle_t c)
{
    c.ca &= ~((1u << CA_CK) | (1u << CA_CKN));
    c.flags &= CY_DRIVE | CY_STROBE | CY_SAMPLE;
    if (!(c.flags & CY_DRIVE)) {
        c.flags &= ~CY_STROBE;
        c.dq[0] = c.dq[1] = 0;
    }
    return c;
}

// Each stream's view of cycle i, as the word it would need (repeats aside)
static uint32_t cmd_word(const ddr3_cycle_t *c, size_t i)
{
    return (pioenc_normalise(c[i]).ca >> CA_CKE) & CA_PIN_MASK;
}

static uint32_t writer_word(const ddr3_cycle_t *c, size_t i)
{
    ddr3_cycle_t x = pioenc_normalise(c[i]);
    if (!(x.flags & CY_DRIVE)) return PIOENC_WR_IDLE;
    uint32_t kind = (x.flags & CY_STROBE) ? PIOENC_WR_STROBE : PIOENC_WR_DRIVE;
    return kind | (uint32_t)x.dq[0] << 2 | (uint32_t)x.dq[1] << 10;
}

static uint32_t sampler_word(const ddr3_cycle_t *c, size_t i)
{
    uint32_t w = (c[i].flags & CY_SAMPLE) ? PIOENC_SMP_BEAT0 : 0;
    if (i && (c[i - 1].flags & CY_SAMPLE)) w |= PIOENC_SMP_BEAT1;
    return w;
}

// Run-length encode one view: a run of equal words becomes the word plus a
// repeat count at rep_shift (max rep_max); can_repeat says whether a word may
// carry one at all (drive words use those bits for data).
static size_t encode(const ddr3_cycle_t *c, size_t n, uint32_t (*view)(const ddr3_cycle_t *, size_t),
                     bool (*can_repeat)(uint32_t), unsigned rep_shift, uint32_t rep_max,
                     uint32_t *out, size_t cap, bool *overflow)
{
    size_t w = 0;
    *overflow = false;
    for (size_t i = 0; i < n;) {
        uint32_t x = view(c, i), rep = 0;
        if (can_repeat(x))
            while (i + 1 + rep < n && rep < rep_max && view(c, i + 1 + rep) == x) rep++;
        if (w == cap) {
            *overflow = true;
            return w;
        }
        out[w++] = x | rep << rep_shift;
        i += 1 + rep;
    }
    return w;
}

static bool always(uint32_t w) { (void)w; return true; }
static bool idle_only(uint32_t w) { return (w & 3u) == PIOENC_WR_IDLE; }

size_t pioenc_cmd(const ddr3_cycle_t *c, size_t n, uint32_t *out, size_t cap, bool *overflow)
{
    return encode(c, n, cmd_word, always, PIOENC_REP_SHIFT, PIOENC_REP_MAX, out, cap, overflow);
}

size_t pioenc_writer(const ddr3_cycle_t *c, size_t n, uint32_t *out, size_t cap, bool *overflow)
{
    return encode(c, n, writer_word, idle_only, 2, PIOENC_REP16_MAX, out, cap, overflow);
}

size_t pioenc_sampler(const ddr3_cycle_t *c, size_t n, uint32_t *out, size_t cap, bool *overflow)
{
    return encode(c, n, sampler_word, always, 2, PIOENC_REP16_MAX, out, cap, overflow);
}

size_t pioenc_decode(const uint32_t *cmd, size_t n_cmd, const uint32_t *wr, size_t n_wr,
                     const uint32_t *smp, size_t n_smp, ddr3_cycle_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t w = 0; w < n_cmd; w++)
        for (uint32_t k = 0; k <= cmd[w] >> PIOENC_REP_SHIFT; k++) {
            if (n == cap) return 0;
            out[n++] = (ddr3_cycle_t){ .ca = (cmd[w] & CA_PIN_MASK) << CA_CKE };
        }
    size_t m = 0;
    for (size_t w = 0; w < n_wr; w++) {
        uint32_t kind = wr[w] & 3u;
        if (kind == 3u) return 0;
        uint32_t rep = kind == PIOENC_WR_IDLE ? (wr[w] >> 2) & 0xFFFFu : 0;
        for (uint32_t k = 0; k <= rep; k++, m++) {
            if (m >= n) return 0;
            if (kind != PIOENC_WR_IDLE) {
                out[m].flags |= CY_DRIVE | (kind == PIOENC_WR_STROBE ? CY_STROBE : 0);
                out[m].dq[0] = (uint8_t)(wr[w] >> 2);
                out[m].dq[1] = (uint8_t)(wr[w] >> 10);
            }
        }
    }
    if (m != n) return 0;
    m = 0;
    bool prev = false;
    for (size_t w = 0; w < n_smp; w++)
        for (uint32_t k = 0; k <= ((smp[w] >> 2) & 0xFFFFu); k++, m++) {
            if (m >= n) return 0;
            bool b1 = smp[w] & PIOENC_SMP_BEAT1;
            if (b1 != prev) return 0;   // BEAT1 must follow a sampled cycle exactly
            if (smp[w] & PIOENC_SMP_BEAT0) out[m].flags |= CY_SAMPLE;
            prev = smp[w] & PIOENC_SMP_BEAT0;
        }
    return m == n ? n : 0;
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

void pioenc_executor_timing(uint32_t tck_ns, uint32_t sysclk_hz, pioenc_timing_t *out)
{
    uint64_t d = (uint64_t)tck_ns * sysclk_hz / (8ull * 1000000000ull);
    out->div = d < 1 ? 1 : d > 65535 ? 65535 : (uint32_t)d;
    out->delay = 0;
    out->tck_ps = (uint32_t)(8ull * out->div * 1000000000000ull / sysclk_hz);
}
