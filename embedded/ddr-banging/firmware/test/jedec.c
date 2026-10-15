#include "jedec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pins.h"
#include "sim.h"

// DDR3-1333 (-H9) minimums, ns, and DLL-off latencies
static double T_REFI = 7800.0;   // 3900 above 85 C: jedec_set_hot()
#define T_RFC    110.0      // 1 Gb
#define T_RCD    13.5
#define T_RP     13.5
#define T_RAS    36.0
#define T_RC     49.5
#define T_RRD    6.0        // and >= 4 CK
#define T_FAW    30.0
#define T_WR     15.0
#define T_WTR    7.5        // and >= 4 CK
#define T_RTP    7.5        // and >= 4 CK
#define T_MOD    15.0       // and >= 12 CK
#define T_XS     (T_RFC + 10.0)   // and >= 5 CK
#define CK_MRD   4
#define CK_CCD   4
#define CK_CKESR 4          // tCKE(min) + 1 CK
#define CK_XSDLL 512
#define CK_ZQINIT 512
#define RL       DDR3_RL_DLL_OFF
#define WL       DDR3_CWL
#define MAX_POSTPONED 8

typedef struct {
    bool open;
    double t_act, t_pre, t_wr_end, t_rd;
} bank_t;

typedef struct {
    double tck;
    int violations;
    long cycle;
    bool cke, in_sr, powered_up;   // powered_up: CKE has gone high once
    bool mrs_done;                  // init: the first MRS has been seen
    double t_cke_up, t_sre, t_srx;
    double t_ref, t_mrs, t_zq, t_col, t_rd_any, t_wr_end_any;
    double t_act[4];                // last four ACTs, any bank (tRRD, tFAW)
    bank_t b[8];
    double refresh_epoch;           // refresh counting restarts here
    long intervals;                 // whole tREFIs since the epoch
    long postponed;                 // REFs owed; negative = pulled in (credit capped at 8)
    double ref_times[16];           // the last 16 REFs (at most 16 in any 2 x tREFI)
    bool owed_flagged;
} st_t;

static const double NEVER = -1e18;

void jedec_set_hot(bool hot)
{
    T_REFI = hot ? 3900.0 : 7800.0;
}

static void fail(st_t *s, double t, const char *what, double got, double need)
{
    if (s->violations++ < 8)
        printf("    jedec: cycle %ld (t = %.0f ns): %s (%.1f ns, need %.1f)\n",
               s->cycle, t, what, got, need);
}

static void need(st_t *s, double t, double since, double min, const char *what)
{
    if (t - since < min - 1e-6) fail(s, t, what, t - since, min);
}

static double ck_max(const st_t *s, double ns, int ck)
{
    return fmax(ns, ck * s->tck);
}

static void init(st_t *s, double tck, bool in_sr)
{
    memset(s, 0, sizeof *s);
    s->tck = tck;
    s->in_sr = in_sr;
    s->powered_up = in_sr;
    s->mrs_done = in_sr;
    s->t_cke_up = s->t_sre = s->t_srx = s->t_ref = s->t_mrs = s->t_zq = NEVER;
    s->t_col = s->t_rd_any = s->t_wr_end_any = NEVER;
    for (int k = 0; k < 4; k++) s->t_act[k] = NEVER;
    for (int k = 0; k < 16; k++) s->ref_times[k] = NEVER;
    for (int k = 0; k < 8; k++)
        s->b[k] = (bank_t){ false, NEVER, NEVER, NEVER, NEVER };
    s->refresh_epoch = NEVER;
    if (in_sr) s->t_sre = -1e9;   // long since
}

static ddr3_cmd_t decode(uint32_t w)
{
    if ((w >> CA_CS0N) & 1) return DDR3_DES;
    unsigned k = ((w >> CA_RASN) & 1) << 2 | ((w >> CA_CASN) & 1) << 1 | ((w >> CA_WEN) & 1);
    static const ddr3_cmd_t cmds[8] = { DDR3_MRS, DDR3_REF, DDR3_PRE, DDR3_ACT,
                                        DDR3_WR, DDR3_RD, DDR3_ZQCL, DDR3_NOP };
    return cmds[k];
}

static bool all_closed(const st_t *s)
{
    for (int k = 0; k < 8; k++)
        if (s->b[k].open) return false;
    return true;
}

static void restart_refresh(st_t *s, double epoch)
{
    s->refresh_epoch = epoch;
    s->intervals = 0;
    s->postponed = 0;
}

static void check_refresh_owed(st_t *s, double t)
{
    if (s->in_sr || !s->cke || s->refresh_epoch == NEVER || t < s->refresh_epoch) return;
    long n = (long)floor((t - s->refresh_epoch) / T_REFI);
    s->postponed += n - s->intervals;
    s->intervals = n;
    if (s->postponed > MAX_POSTPONED) {
        if (!s->owed_flagged) fail(s, t, "more than 8 REFs postponed", (double)s->postponed, 8);
        s->owed_flagged = true;
    } else {
        s->owed_flagged = false;
    }
}

static void count_ref(st_t *s, double t)
{
    check_refresh_owed(s, t);
    // JEDEC credits at most 8 REFs issued early; more are simply wasted
    if (--s->postponed < -MAX_POSTPONED) s->postponed = -MAX_POSTPONED;
    need(s, t, s->ref_times[0], 2 * T_REFI, "more than 16 REFs in 2 x tREFI");
    memmove(s->ref_times, s->ref_times + 1, 15 * sizeof s->ref_times[0]);
    s->ref_times[15] = t;
}

// The DRAM registers `ca` on the rising CK edge at time t.
static void edge(st_t *s, double t, uint32_t ca)
{
    bool cke = (ca >> CA_CKE) & 1;
    ddr3_cmd_t c = decode(ca);
    bool idle_cmd = c == DDR3_NOP || c == DDR3_DES;
    unsigned ba = (ca >> CA_BA0) & 7u, a = (ca >> CA_A0) & 0x3FFFu;

    // ---- CKE transitions -------------------------------------------------
    if (cke && !s->cke) {
        if (!idle_cmd) fail(s, t, "command on the CKE-high edge", 0, 0);
        if (s->in_sr) {                          // self-refresh exit
            need(s, t, s->t_sre, CK_CKESR * s->tck, "tCKESR: self-refresh too short");
            s->in_sr = false;
            s->t_srx = t;
        } else if (!s->powered_up) {             // power-up: CKE high for the first time
            s->powered_up = true;
        } else {
            fail(s, t, "CKE rose without self-refresh (power-down is unused)", 0, 0);
        }
        s->t_cke_up = t;
        if (s->t_srx == t) restart_refresh(s, t);   // counting restarts after self-refresh
    } else if (!cke && s->cke) {
        if (c != DDR3_REF) fail(s, t, "CKE fell without SRE (power-down is unused)", 0, 0);
        else {                                   // self-refresh entry
            if (!all_closed(s)) fail(s, t, "SRE with a bank open", 0, 0);
            for (int k = 0; k < 8; k++) need(s, t, s->b[k].t_pre, T_RP, "tRP before SRE");
            need(s, t, s->t_ref, T_RFC, "tRFC before SRE");
            need(s, t, s->t_mrs, ck_max(s, T_MOD, 12), "tMOD before SRE");
            s->in_sr = true;
            s->t_sre = t;
        }
        s->cke = cke;
        return;
    } else if (!cke) {
        if (!idle_cmd) fail(s, t, "command with CKE low", 0, 0);
        return;
    }
    s->cke = cke;
    check_refresh_owed(s, t);
    if (idle_cmd) return;

    // ---- rules for every command -------------------------------------------
    if (!s->mrs_done && c != DDR3_MRS) fail(s, t, "command before the mode registers are set", 0, 0);
    if (s->t_srx != NEVER) {
        need(s, t, s->t_srx, ck_max(s, T_XS, 5), "tXS after self-refresh exit");
        if (c == DDR3_RD) need(s, t, s->t_srx, CK_XSDLL * s->tck, "tXSDLL before READ");
    }
    need(s, t, s->t_zq, CK_ZQINIT * s->tck, "tZQinit after ZQCL");
    need(s, t, s->t_ref, T_RFC, "tRFC after REF");
    if (c != DDR3_MRS) need(s, t, s->t_mrs, ck_max(s, T_MOD, 12), "tMOD after MRS");

    switch (c) {
    case DDR3_MRS:
        if (!s->mrs_done) need(s, t, s->t_cke_up, ck_max(s, T_XS, 5), "tXPR before the first MRS");
        need(s, t, s->t_mrs, CK_MRD * s->tck, "tMRD between MRS");
        if (!all_closed(s)) fail(s, t, "MRS with a bank open", 0, 0);
        s->mrs_done = true;
        s->t_mrs = t;
        break;
    case DDR3_ZQCL:
        if (!all_closed(s)) fail(s, t, "ZQCL with a bank open", 0, 0);
        s->t_zq = t;
        if (s->refresh_epoch == NEVER)   // power-up: counting starts after tZQinit
            restart_refresh(s, t + CK_ZQINIT * s->tck);
        break;
    case DDR3_REF:
        if (!all_closed(s)) fail(s, t, "REF with a bank open", 0, 0);
        for (int k = 0; k < 8; k++) need(s, t, s->b[k].t_pre, T_RP, "tRP before REF");
        s->t_ref = t;
        count_ref(s, t);
        break;
    case DDR3_ACT: {
        bank_t *b = &s->b[ba];
        if (b->open) fail(s, t, "ACT to an open bank", 0, 0);
        need(s, t, b->t_pre, T_RP, "tRP");
        need(s, t, b->t_act, T_RC, "tRC");
        need(s, t, s->t_act[3], ck_max(s, T_RRD, 4), "tRRD");
        need(s, t, s->t_act[0], T_FAW, "tFAW");
        memmove(s->t_act, s->t_act + 1, 3 * sizeof s->t_act[0]);
        s->t_act[3] = t;
        b->open = true;
        b->t_act = t;
        b->t_rd = b->t_wr_end = NEVER;
        break;
    }
    case DDR3_RD:
    case DDR3_WR: {
        bank_t *b = &s->b[ba];
        if (!b->open) fail(s, t, c == DDR3_RD ? "READ to a closed bank" : "WRITE to a closed bank", 0, 0);
        if (a & (1u << 10)) fail(s, t, "auto-precharge (unused, unchecked)", 0, 0);
        need(s, t, b->t_act, T_RCD, "tRCD");
        need(s, t, s->t_col, CK_CCD * s->tck, "tCCD");
        if (c == DDR3_RD) {
            need(s, t, s->t_wr_end_any, ck_max(s, T_WTR, 4), "tWTR");
            b->t_rd = s->t_rd_any = t;
        } else {
            need(s, t, s->t_rd_any, (RL + CK_CCD + 2 - WL) * s->tck, "READ to WRITE");
            b->t_wr_end = s->t_wr_end_any = t + (WL + 4) * s->tck;
        }
        s->t_col = t;
        break;
    }
    case DDR3_PRE:
        for (int k = 0; k < 8; k++) {
            bank_t *b = &s->b[k];
            if (!((a & (1u << 10)) || k == (int)ba) || !b->open) continue;
            need(s, t, b->t_act, T_RAS, "tRAS");
            if (t - b->t_act > 9 * T_REFI) fail(s, t, "tRAS(max): row open too long", t - b->t_act, 9 * T_REFI);
            need(s, t, b->t_rd, ck_max(s, T_RTP, 4), "tRTP");
            if (b->t_wr_end != NEVER) need(s, t, b->t_wr_end, T_WR, "tWR");
            b->open = false;
            b->t_pre = t;
        }
        break;
    default:
        break;
    }
}

static void finish(st_t *s, double t)
{
    for (int k = 0; k < 8; k++)
        if (s->b[k].open) fail(s, t, "sequence ends with a row open", t - s->b[k].t_act, 0);
    if (s->cke) fail(s, t, "sequence ends out of self-refresh", 0, 0);
}

int jedec_check_seq(const ddr3_seq_t *seq, bool in_sr)
{
    st_t s;
    init(&s, seq->tck_ns, in_sr);
    for (size_t i = 0; i < seq->len; i++) {
        s.cycle = (long)i;
        double t = (double)i * seq->tck_ns;
        uint32_t ca = seq->cycles[i].ca;
        edge(&s, t, ca);
        // data: write bursts strobed at WL, read windows covering RL..RL+3
        ddr3_cmd_t c = (ca >> CA_CKE) & 1 ? decode(ca) : DDR3_NOP;
        if (c == DDR3_WR) {
            for (int k = WL - 1; k < WL + 4; k++) {
                uint8_t want = k == WL - 1 ? CY_DRIVE : CY_DRIVE | CY_STROBE;
                if (i + k >= seq->len || (seq->cycles[i + k].flags & want) != want)
                    fail(&s, t, "write data not driven/strobed at WL", k, WL);
            }
        } else if (c == DDR3_RD) {
            for (int k = RL; k < RL + 4; k++)
                if (i + k >= seq->len || !(seq->cycles[i + k].flags & CY_SAMPLE))
                    fail(&s, t, "read data not sampled at RL", k, RL);
        }
    }
    finish(&s, (double)seq->len * seq->tck_ns);
    return s.violations;
}

int jedec_check_pins(uint32_t tck_ns, bool in_sr)
{
    st_t s;
    init(&s, tck_ns, in_sr);
    uint64_t ck = 1ull << (PIN_CA_BASE + CA_CK);
    bool prev = false;
    double t = 0;
    for (size_t i = 0; i < sim_len; i++) {
        bool now = sim_log[i].out & ck;
        if (now && !prev) {
            s.cycle++;
            t = sim_log[i].t * (tck_ns / 4.0);   // one sim tick per quarter clock
            edge(&s, t, (uint32_t)(sim_log[i].out >> PIN_CA_BASE) & ((1u << CA_BITS) - 1));
        }
        prev = now;
    }
    finish(&s, t);
    return s.violations;
}
