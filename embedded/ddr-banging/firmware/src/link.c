#include "link.h"

#include <string.h>

void link_init(link_t *l, const link_backend_t *be, uint8_t *tx_buf)
{
    memset(l, 0, sizeof *l);
    l->be = *be;
    l->tx = tx_buf;
}

const uint8_t *link_tx(const link_t *l, size_t *len)
{
    *len = l->tx_len;
    return l->tx;
}

static void reply(link_t *l, const ddrlink_hdr_t *cmd, uint8_t status, uint32_t payload)
{
    if (status != DDRLINK_OK) {
        l->errors++;
        l->last_err = status;
        payload = 0;
    }
    ddrlink_hdr_t h = { DDRLINK_MAGIC_RSP, cmd->op, cmd->seq, status, payload };
    ddrlink_put_hdr(l->tx, &h);
    l->tx_len = DDRLINK_HDR;
    if (payload) {
        ddrlink_put32(l->tx + DDRLINK_HDR + payload,
                      ddrlink_crc32(l->tx + DDRLINK_HDR, payload));
        l->tx_len += payload + DDRLINK_CRC;
    }
}

static uint8_t check_range(const link_t *l, uint32_t addr, uint32_t len)
{
    if (addr % DDRLINK_ALIGN || len % DDRLINK_ALIGN) return DDRLINK_E_ALIGN;
    if (len == 0 || len > DDRLINK_MAX_LEN || addr > l->be.capacity || len > l->be.capacity - addr)
        return DDRLINK_E_RANGE;
    if (!l->be.initialised(l->be.ctx)) return DDRLINK_E_NOT_INIT;
    return DDRLINK_OK;
}

static void status(link_t *l, const ddrlink_hdr_t *cmd)
{
    ddrlink_status_t s = {
        .version = DDRLINK_VERSION,
        .flags = l->be.initialised(l->be.ctx) ? DDRLINK_F_INITIALISED : 0,
        .capacity = l->be.capacity,
        .tck_ns = l->be.tck_ns(l->be.ctx),
        .temp16 = l->be.temp16(l->be.ctx),
        .last_err = l->last_err,
        .frames = l->frames,
        .errors = l->errors,
    };
    ddrlink_put_status(l->tx + DDRLINK_HDR, &s);
    reply(l, cmd, DDRLINK_OK, DDRLINK_STATUS_LEN);
}

static void command(link_t *l, const uint8_t *rx, size_t n, const ddrlink_hdr_t *h)
{
    uint8_t st;
    switch (h->op) {
    case DDRLINK_STATUS:
        status(l, h);
        return;
    case DDRLINK_INIT:
        reply(l, h, l->be.init(l->be.ctx) ? DDRLINK_OK : DDRLINK_E_DRAM, 0);
        return;
    case DDRLINK_WRITE: {
        if ((st = check_range(l, h->addr, h->len)) != DDRLINK_OK) break;
        if (n < DDRLINK_HDR + h->len + DDRLINK_CRC) { st = DDRLINK_E_SHORT; break; }
        const uint8_t *d = rx + DDRLINK_HDR;
        if (ddrlink_get32(d + h->len) != ddrlink_crc32(d, h->len)) { st = DDRLINK_E_DATA_CRC; break; }
        st = l->be.write(l->be.ctx, h->addr, d, h->len) ? DDRLINK_OK : DDRLINK_E_DRAM;
        break;
    }
    case DDRLINK_READ:
        if ((st = check_range(l, h->addr, h->len)) != DDRLINK_OK) break;
        if (!l->be.read(l->be.ctx, h->addr, l->tx + DDRLINK_HDR, h->len)) { st = DDRLINK_E_DRAM; break; }
        reply(l, h, DDRLINK_OK, h->len);
        return;
    default:
        st = DDRLINK_E_OP;
    }
    reply(l, h, st, 0);
}

void link_frame(link_t *l, const uint8_t *rx, size_t n)
{
    // The reply stays put until the next command replaces it, so the master
    // can re-read one that arrived corrupt.
    if (n < DDRLINK_HDR) return;
    ddrlink_hdr_t h;
    bool crc_ok = ddrlink_get_hdr(rx, &h);
    if (h.magic != DDRLINK_MAGIC_CMD) return;   // a response frame (the master sent zeros)
    l->frames++;
    if (!crc_ok) {
        // op and seq may be corrupt too; echo them anyway, the CRC'd reply says why
        reply(l, &h, DDRLINK_E_HDR_CRC, 0);
        return;
    }
    command(l, rx, n, &h);
}
