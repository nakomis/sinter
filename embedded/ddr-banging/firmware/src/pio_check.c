#include "pio_check.h"

#include "pins.h"

// GPIO 0-31 as the capture sees them
#define CK_BIT    (1u << (PIN_CA_BASE + CA_CK))     // 16
#define CKN_BIT   (1u << (PIN_CA_BASE + CA_CKN))    // 17
#define CA_VIS    (0xFFFFFFFFu << (PIN_CA_BASE + CA_CKE))   // 18..31: CKE .. A4
#define DIR_BITS  ((1u << PIN_DIR_DQ) | (1u << PIN_DIR_DQS))
#define DQ_BITS   (0xFFu << PIN_DQ0)
#define DQS_BIT   (1u << PIN_DQS)
#define DQSN_BIT  (1u << PIN_DQSN)

size_t pio_check_sequence(ddr3_cycle_t *c, size_t cap)
{
    size_t n = PIO_CHECK_CYCLES < cap ? PIO_CHECK_CYCLES : cap;
    for (size_t i = 0; i < n; i++) {
        // C/A: a different pattern each cycle, CKE always low; cycles 4..11
        // the same, so the cmd SM's repeat path runs
        uint32_t k = (i >= 4 && i < 12) ? 4 : (uint32_t)i;
        uint32_t ca = (k * 0x9E3779B1u) >> 7;
        ca &= CA_MASK & ~(1u << CA_CKE);
        c[i] = (ddr3_cycle_t){ .ca = ca };
        uint8_t d0 = (uint8_t)(0x11 * i + 3), d1 = (uint8_t)(0x5A ^ (i * 7));
        if (i == 15 || i == 32) {                      // preamble / postamble
            c[i].flags = CY_DRIVE;
        } else if (i >= 16 && i < 32) {                // bursts, read back by the sampler
            c[i].flags = CY_DRIVE | CY_STROBE | CY_SAMPLE;
            c[i].dq[0] = d0;
            c[i].dq[1] = d1;
        } else if (i >= 40 && i < 48) {                // driving without strobes
            c[i].flags = CY_DRIVE;
            c[i].dq[0] = d1;
            c[i].dq[1] = d0;
        }
    }
    return n;
}

// The pins after each SM cycle (c0..c7) of each CK, from bus_run's quarter
// table as the three programs implement it (see the .pio comments)
typedef struct {
    uint32_t pins, driven;   // levels; which of DQ/DQS the RP2350 drives
} state_t;

static void step(state_t *s, const ddr3_cycle_t *cy, unsigned c)
{
    bool drive = cy->flags & CY_DRIVE, strobe = drive && (cy->flags & CY_STROBE);
    switch (c) {
    case 0:
        s->pins = (s->pins & ~(CK_BIT | CKN_BIT | CA_VIS)) | CKN_BIT |
                  (((uint32_t)cy->ca << PIN_CA_BASE) & CA_VIS);
        if (drive) s->pins = (s->pins & ~(DQS_BIT | DQSN_BIT)) | DIR_BITS | DQSN_BIT;
        break;
    case 1:
        s->driven = drive ? DQ_BITS | DQS_BIT | DQSN_BIT : 0;
        break;
    case 2:
        if (drive) s->pins = (s->pins & ~DQ_BITS) | (uint32_t)cy->dq[0] << PIN_DQ0;
        else s->pins &= ~(DIR_BITS | DQS_BIT | DQSN_BIT);
        break;
    case 4:
        s->pins = (s->pins & ~(CK_BIT | CKN_BIT)) | CK_BIT;
        if (strobe) s->pins = (s->pins & ~(DQS_BIT | DQSN_BIT)) | DQS_BIT;
        break;
    case 6:
        if (drive) s->pins = (s->pins & ~DQ_BITS) | (uint32_t)cy->dq[1] << PIN_DQ0;
        break;
    default:
        break;
    }
}

bool pio_check(const ddr3_cycle_t *c, size_t n, const uint32_t *capture, size_t n_capture,
               const bus_sample_t *samples, size_t n_samples, pio_check_result_t *r)
{
    *r = (pio_check_result_t){ .offset = -1 };
    // CK 0 rises after its c4: the first rising edge in the capture
    for (size_t k = 1; k < n_capture; k++)
        if ((capture[k] & CK_BIT) && !(capture[k - 1] & CK_BIT)) {
            r->offset = (int)k - 4;
            break;
        }
    if (r->offset < 0 || (size_t)r->offset + PIO_CHECK_SM_PER_CK * n > n_capture) {
        r->pin_errors = 1;   // no clock, or not enough capture: nothing lines up
        return false;
    }

    state_t s = { .pins = CKN_BIT, .driven = 0 };
    for (size_t i = 0; i < n; i++)
        for (unsigned cc = 0; cc < PIO_CHECK_SM_PER_CK; cc++) {
            step(&s, &c[i], cc);
            uint32_t mask = CK_BIT | CKN_BIT | CA_VIS | DIR_BITS | s.driven;
            uint32_t got = capture[r->offset + PIO_CHECK_SM_PER_CK * i + cc];
            if ((got ^ s.pins) & mask) {
                if (!r->pin_errors) {
                    r->first_bad = (unsigned)(PIO_CHECK_SM_PER_CK * i + cc);
                    r->first_want = s.pins & mask;
                    r->first_got = got & mask;
                    r->first_mask = mask;
                }
                r->pin_errors++;
            }
        }

    // The sampler reads the pads at c6 (beat 0) and the next cycle's c2
    // (beat 1), after c5 and c1 have taken effect: for a strobed burst that
    // is dq[0] with DQS high, then dq[1] with DQS low, exactly what a DRAM
    // returns. Checked where this cycle strobes and the next still drives.
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (!(c[i].flags & CY_SAMPLE)) continue;
        if (j >= n_samples) {
            r->loop_errors++;
            break;
        }
        bool next_drives = i + 1 < n && (c[i + 1].flags & CY_DRIVE);
        if ((c[i].flags & CY_STROBE) && next_drives) {
            uint16_t b0 = (uint16_t)(c[i].dq[0] | DQS_BIT);
            uint16_t b1 = (uint16_t)(c[i].dq[1] | DQSN_BIT);
            r->loop_errors += (samples[j].beat[0] & 0x3FF) != b0;
            r->loop_errors += (samples[j].beat[1] & 0x3FF) != b1;
            r->samples += 2;
        }
        j++;
    }
    return !r->pin_errors && !r->loop_errors && r->samples;
}
