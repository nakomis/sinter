// ddrlink: the ESP32 <-> RP2350B command protocol (SINT-100 milestone 10).
// Shared, byte for byte, by the RP2350 firmware (slave) and the ESP32
// client (master). Pure C99, no hardware.
//
// Physical: SPI mode 3, MSB first, ESP32 master. Every exchange is one
// CS-low frame. The RP2350 holds READY low from the start of each frame until
// it has finished acting on it; the master waits for READY high before every
// frame. Frames are at least DDRLINK_HDR bytes long, so READY is already low
// by the time CS rises.
//
// Two kinds of frame, told apart by the first bytes the master sends:
//
//   command   master sends a header (+ for WRITE: len data bytes, then the
//             CRC32 of the data). What the slave sends meanwhile is ignored.
//   response  master sends zeros and clocks in the slave's reply to the last
//             command: a header (+ for READ/STATUS: len data bytes and their
//             CRC32). The reply stays until the next command replaces
//             it, so a master that got it corrupt just reads it again.
//
// All multi-byte fields are little-endian.

#ifndef DDRLINK_H
#define DDRLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DDRLINK_VERSION   1
#define DDRLINK_HDR       16          // header bytes, both directions
#define DDRLINK_CRC       4           // trailing CRC32 after a data payload
#define DDRLINK_MAX_LEN   65536u      // largest READ/WRITE payload
#define DDRLINK_ALIGN     8u          // one BL8 burst on one x8 chip
#define DDRLINK_FRAME_MAX (DDRLINK_HDR + DDRLINK_MAX_LEN + DDRLINK_CRC)

#define DDRLINK_MAGIC_CMD 0x4244u     // "DB" on the wire
#define DDRLINK_MAGIC_RSP 0x5244u     // "DR"

typedef enum {
    DDRLINK_STATUS = 1,   // no payload; reply carries ddrlink_status_t
    DDRLINK_INIT   = 2,   // reset + DLL-off init; reply has no payload
    DDRLINK_WRITE  = 3,   // payload: len bytes + CRC32; reply has no payload
    DDRLINK_READ   = 4,   // no payload; reply carries len bytes + CRC32
} ddrlink_op_t;

typedef enum {
    DDRLINK_OK = 0,
    DDRLINK_E_HDR_CRC,    // command header CRC mismatch
    DDRLINK_E_DATA_CRC,   // WRITE payload CRC mismatch; nothing written
    DDRLINK_E_OP,         // unknown op
    DDRLINK_E_ALIGN,      // addr or len not a multiple of DDRLINK_ALIGN
    DDRLINK_E_RANGE,      // len 0 or > DDRLINK_MAX_LEN, or past the end of memory
    DDRLINK_E_SHORT,      // frame ended before the payload did
    DDRLINK_E_NOT_INIT,   // READ/WRITE before INIT
    DDRLINK_E_DRAM,       // the memory side failed (sequence overflow etc.)
} ddrlink_err_t;

// Header, both directions. Command: magic CMD, op, seq, addr, len.
// Response: magic RSP, op and seq echoed, status = ddrlink_err_t, len = payload
// bytes that follow (0 on error). crc = CRC32 of the first 12 bytes.
typedef struct {
    uint16_t magic;
    uint8_t op;
    uint8_t seq;
    uint32_t addr;        // response: low byte = status, rest zero
    uint32_t len;
} ddrlink_hdr_t;

// STATUS reply payload (DDRLINK_STATUS_LEN bytes on the wire).
typedef struct {
    uint16_t version;     // DDRLINK_VERSION
    uint16_t flags;       // DDRLINK_F_*
    uint32_t capacity;    // bytes addressable
    uint32_t tck_ns;      // memory clock period
    int16_t temp16;       // module temperature, 1/16 C; DDRLINK_NO_TEMP if none
    uint8_t last_err;     // last non-OK status returned
    uint8_t reserved;
    uint32_t frames;      // command frames seen
    uint32_t errors;      // command frames that failed
} ddrlink_status_t;

#define DDRLINK_STATUS_LEN 24
#define DDRLINK_F_INITIALISED 0x0001u
#define DDRLINK_NO_TEMP INT16_MIN

uint32_t ddrlink_crc32(const uint8_t *p, size_t n);
uint32_t ddrlink_crc32_update(uint32_t crc, const uint8_t *p, size_t n);  // start with 0

void ddrlink_put_hdr(uint8_t out[DDRLINK_HDR], const ddrlink_hdr_t *h);   // fills crc
// false if the CRC doesn't match (fields are still filled in)
bool ddrlink_get_hdr(const uint8_t in[DDRLINK_HDR], ddrlink_hdr_t *h);

void ddrlink_put_status(uint8_t out[DDRLINK_STATUS_LEN], const ddrlink_status_t *s);
void ddrlink_get_status(const uint8_t in[DDRLINK_STATUS_LEN], ddrlink_status_t *s);

void ddrlink_put32(uint8_t *p, uint32_t v);
uint32_t ddrlink_get32(const uint8_t *p);

// Bytes the master must clock in a response frame for this command, assuming
// it succeeds (on error the reply is just the header).
size_t ddrlink_response_len(uint8_t op, uint32_t len);

const char *ddrlink_err_str(uint8_t err);

#endif
