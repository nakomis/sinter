// DDR3 command encoding and cycle-by-cycle sequence building.
//
// Pure logic, no hardware calls: everything here is unit-tested on the host.
// A sequence is a list of CK cycles; an executor (bus_gpio.c now, PIO later)
// plays it out. Cycle n's command/address is presented while CK is low and
// registered by the DRAM on the rising edge that ends the low half.
//
// Mode: DLL off (MR1 A0=1), CL=6, CWL=6, AL=0, BL8 fixed, ODT off.
// Target: one x8 chip of a 2Rx8 UDIMM (Samsung K4B1G0846F: 8 banks,
// 14 row bits, 10 column bits, 128 MB). Rank 0 only; CS1# held high.

#ifndef DDR3_H
#define DDR3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- Command/address word ----------------------------------------------
// Bit positions relative to GPIO 16 (the channel board pin map, DESIGN.md §2),
// so a word can go straight to a PIO state machine based at GPIO 16.
#define CA_BASE_GPIO 16
#define CA_CK    0
#define CA_CKN   1
#define CA_CKE   2
#define CA_CS0N  3
#define CA_CS1N  4
#define CA_RASN  5
#define CA_CASN  6
#define CA_WEN   7
#define CA_BA0   8   // BA0..BA2 = bits 8..10
#define CA_A0    11  // A0..A13  = bits 11..24
#define CA_BITS  25

// Everything except CK/CK# (the executor drives those).
#define CA_MASK  (((1u << CA_BITS) - 1) & ~((1u << CA_CK) | (1u << CA_CKN)))

typedef enum {
    DDR3_DES,     // deselect (CS# high)
    DDR3_NOP,
    DDR3_MRS,     // ba = mode register, addr = opcode
    DDR3_REF,
    DDR3_PRE,     // addr bit 10 set = all banks
    DDR3_ACT,     // ba, addr = row
    DDR3_WR,      // ba, addr = column (A10 = auto-precharge)
    DDR3_RD,
    DDR3_ZQCL,
} ddr3_cmd_t;

// The CA word for a command. CKE is taken from `cke`; CS1# is always high.
uint32_t ddr3_encode(ddr3_cmd_t cmd, unsigned ba, unsigned addr, bool cke);

// ---- Mode registers -----------------------------------------------------
#define DDR3_CL   6
#define DDR3_CWL  6
// DLL-off read latency: data starts CL + AL - 1 clocks after READ (Micron
// 1 Gb DDR3 Rev. L, plus tDQSCK(DLL_DIS) of 1..10 ns, negligible here).
#define DDR3_RL_DLL_OFF (DDR3_CL - 1)

uint16_t ddr3_mr0(void);  // BL8, sequential, CL6, no DLL reset, WR=6
uint16_t ddr3_mr1(void);  // DLL off, RZQ/6 drive, RTT_NOM off, AL=0
uint16_t ddr3_mr2(void);  // CWL6, RTT_WR off
uint16_t ddr3_mr3(void);  // 0

// ---- Addressing ---------------------------------------------------------
// Linear byte address within one chip: col = a[9:0], bank = a[12:10],
// row = a[26:13]. Bursts are 8 bytes, so col[2:0] must be 0.
#define DDR3_ROW_BITS  14
#define DDR3_COL_BITS  10
#define DDR3_BANK_BITS 3
#define DDR3_CHIP_BYTES (1u << (DDR3_ROW_BITS + DDR3_COL_BITS + DDR3_BANK_BITS))

typedef struct { unsigned bank, row, col; } ddr3_addr_t;
ddr3_addr_t ddr3_split(uint32_t byte_addr);

// ---- Sequences ----------------------------------------------------------
// Per-cycle data actions.
#define CY_DRIVE   0x01  // RP2350 drives DQ/DQS this cycle (translators A->B)
#define CY_STROBE  0x02  // DQS follows CK (rises with CK, falls with CK)
#define CY_SAMPLE  0x04  // sample DQ/DQS mid-high (beat 0) and mid-low after (beat 1)

typedef struct {
    uint32_t ca;     // CA word for this cycle (CK bits ignored)
    uint8_t  flags;
    uint8_t  dq[2];  // write data: beat on the rising, then falling, DQS edge
} ddr3_cycle_t;

typedef struct {
    ddr3_cycle_t *cycles;
    size_t cap, len;
    bool overflow;
    bool cke;
    uint32_t tck_ns;           // clock period
    uint32_t since_ref;        // refresh debt in cycles (each REF pays off one tREFI)
    uint32_t ref_hold;         // cycles until tRFC of the last REF has passed
    uint32_t ref_interval;     // cycles per tREFI
    uint8_t  open_banks;       // bitmask of banks with an open row
    size_t   last_read;        // first sampled cycle of the latest read
} ddr3_seq_t;

void ddr3_seq_init(ddr3_seq_t *s, ddr3_cycle_t *buf, size_t cap, uint32_t tck_ns);

// Append one cycle carrying `cmd`. Lower-level than the helpers below.
void ddr3_seq_cmd(ddr3_seq_t *s, ddr3_cmd_t cmd, unsigned ba, unsigned addr);

// Idle for n cycles (NOPs). While every bank is closed, REFs are slotted in
// whenever one is due, so the average tREFI is honoured however long the gap.
void ddr3_seq_idle(ddr3_seq_t *s, uint32_t n);

// Cycles needed to cover `ns`, at least `min_ck`.
uint32_t ddr3_ck(const ddr3_seq_t *s, uint32_t ns, uint32_t min_ck);

// Power-up and mode-register initialisation (CKE goes high here). RESET#
// and its 200 us / 500 us waits happen before this, in wall-clock time.
void ddr3_seq_init_mrs(ddr3_seq_t *s);

// One BL8 write or read of 8 bytes at a burst-aligned byte address:
// ACT, WR/RD, data, PRE. A read flags sampling over a window wider than the
// burst, from RL-1 to RL+5, so bring-up can see exactly where data lands.
void ddr3_seq_write(ddr3_seq_t *s, uint32_t byte_addr, const uint8_t data[8]);
void ddr3_seq_read(ddr3_seq_t *s, uint32_t byte_addr);
#define DDR3_READ_WINDOW_BEFORE 1   // cycles sampled before RL
#define DDR3_READ_WINDOW_CYCLES 7   // cycles sampled in all

// With every bank closed, issue every REF owed (then wait out the last
// tRFC). Between back-to-back bursts there is no idle gap for
// ddr3_seq_idle() to slot REFs into, so callers batching bursts use this.
void ddr3_seq_catch_up(ddr3_seq_t *s);

// Index of the first sampled cycle of the most recent read (for decoding).
size_t ddr3_seq_last_read_start(const ddr3_seq_t *s);

// Self-refresh entry/exit. Entry first pays off any postponed REFs; exit
// waits tXSDLL (512 CK) with REFs.
void ddr3_seq_sre(ddr3_seq_t *s);
void ddr3_seq_srx(ddr3_seq_t *s);

#endif
