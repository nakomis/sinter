// ESP32 link, slave side: acts on complete SPI frames (see proto/ddrlink.h).
// Pure logic. link_spi.c moves the bytes; a backend does the memory work.

#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddrlink.h"

typedef struct {
    bool (*init)(void *ctx);
    // addr and len are multiples of DDRLINK_ALIGN and inside capacity
    bool (*write)(void *ctx, uint32_t addr, const uint8_t *d, uint32_t len);
    bool (*read)(void *ctx, uint32_t addr, uint8_t *d, uint32_t len);
    bool (*initialised)(void *ctx);
    uint32_t (*tck_ns)(void *ctx);
    int16_t (*temp16)(void *ctx);   // DDRLINK_NO_TEMP if there's no sensor
    uint32_t capacity;
    void *ctx;
} link_backend_t;

typedef struct {
    link_backend_t be;
    uint8_t *tx;          // reply buffer, DDRLINK_FRAME_MAX bytes
    size_t tx_len;        // reply to the last command; 0 = none yet
    uint32_t frames, errors;
    uint8_t last_err;
} link_t;

void link_init(link_t *l, const link_backend_t *be, uint8_t *tx_buf);

// One CS-low frame has ended and the master sent rx[0..n). Acts on a
// command, if that's what the frame was, and prepares its reply.
void link_frame(link_t *l, const uint8_t *rx, size_t n);

// What the slave should send in the next frame.
const uint8_t *link_tx(const link_t *l, size_t *len);

#endif
