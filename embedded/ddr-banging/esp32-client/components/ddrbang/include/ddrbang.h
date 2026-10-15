// ddrbang: an ESP32's view of a DDR3 DIMM driven by an RP2350B (SINT-100).
// Master side of proto/ddrlink.h. The core here is transport-agnostic;
// ddrbang_spi.c supplies an ESP-IDF SPI transport.

#ifndef DDRBANG_H
#define DDRBANG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddrlink.h"

typedef struct {
    // One full-duplex CS-low frame of n bytes. rx may be NULL.
    bool (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n);
    // Block until READY is high; false on timeout.
    bool (*wait_ready)(void *ctx, uint32_t timeout_ms);
    void *ctx;
    // Frame buffers, DDRLINK_FRAME_MAX bytes each (DMA-capable on the ESP32).
    uint8_t *tx_buf, *rx_buf;
} ddrbang_transport_t;

typedef enum {
    DDRBANG_OK = 0,
    DDRBANG_E_TIMEOUT = -1,   // READY never came back
    DDRBANG_E_XFER = -2,      // the transport failed
    DDRBANG_E_REPLY = -3,     // reply header or payload CRC bad, or seq/op mismatch
    DDRBANG_E_ARG = -4,       // misaligned or too long, caught before sending
    // > 0: the RP2350 answered with that ddrlink_err_t
} ddrbang_err_t;

typedef struct {
    ddrbang_transport_t t;
    uint8_t seq;
    uint32_t timeout_ms;      // per READY wait; INIT and 64 KB ops are slow
    unsigned retries;         // frames and commands re-sent after link errors
} ddrbang_t;

void ddrbang_init(ddrbang_t *d, const ddrbang_transport_t *t);

int ddrbang_status(ddrbang_t *d, ddrlink_status_t *out);
int ddrbang_dram_init(ddrbang_t *d);
// Any length that's a multiple of DDRLINK_ALIGN; split into DDRLINK_MAX_LEN frames.
int ddrbang_write(ddrbang_t *d, uint32_t addr, const void *src, size_t len);
int ddrbang_read(ddrbang_t *d, uint32_t addr, void *dst, size_t len);

const char *ddrbang_strerror(int err);

#endif
