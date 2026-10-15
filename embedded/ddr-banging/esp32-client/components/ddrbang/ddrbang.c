#include "ddrbang.h"

#include <string.h>

#define RESPONSE_TRIES 3   // response frames per command, if a reply arrives corrupt
#define COMMAND_TRIES 3    // whole commands, if the link (not the request) was at fault

void ddrbang_init(ddrbang_t *d, const ddrbang_transport_t *t)
{
    memset(d, 0, sizeof *d);
    d->t = *t;
    d->timeout_ms = 2000;
}

// Send one command frame, then fetch its reply. On success the reply's
// payload (if any) is at rx_buf + DDRLINK_HDR. Returns 0, a negative
// ddrbang_err_t, or the slave's ddrlink_err_t.
static int exchange(ddrbang_t *d, uint8_t op, uint32_t addr, uint32_t len,
                    const uint8_t *payload, uint32_t payload_len)
{
    ddrbang_transport_t *t = &d->t;
    uint8_t seq = ++d->seq;
    ddrlink_hdr_t h = { DDRLINK_MAGIC_CMD, op, seq, addr, len };
    size_t n = DDRLINK_HDR;
    ddrlink_put_hdr(t->tx_buf, &h);
    if (payload_len) {
        memcpy(t->tx_buf + n, payload, payload_len);
        ddrlink_put32(t->tx_buf + n + payload_len, ddrlink_crc32(payload, payload_len));
        n += payload_len + DDRLINK_CRC;
    }
    if (!t->wait_ready(t->ctx, d->timeout_ms)) return DDRBANG_E_TIMEOUT;
    if (!t->xfer(t->ctx, t->tx_buf, NULL, n)) return DDRBANG_E_XFER;

    size_t want = ddrlink_response_len(op, len);
    memset(t->tx_buf, 0, want);
    for (int tries = 0;; tries++) {
        if (!t->wait_ready(t->ctx, d->timeout_ms)) return DDRBANG_E_TIMEOUT;
        if (!t->xfer(t->ctx, t->tx_buf, t->rx_buf, want)) return DDRBANG_E_XFER;
        ddrlink_hdr_t r;
        bool ok = ddrlink_get_hdr(t->rx_buf, &r) && r.magic == DDRLINK_MAGIC_RSP &&
                  r.op == op && r.seq == seq;
        uint32_t status = r.addr;
        if (ok && status == DDRLINK_OK) {
            // the slave only sends a payload on success, of the size asked for
            uint32_t expect = (uint32_t)(want - DDRLINK_HDR - (want > DDRLINK_HDR ? DDRLINK_CRC : 0));
            const uint8_t *p = t->rx_buf + DDRLINK_HDR;
            ok = r.len == expect &&
                 (r.len == 0 || ddrlink_get32(p + r.len) == ddrlink_crc32(p, r.len));
            if (ok) {
                return DDRBANG_OK;
            }
        } else if (ok) {
            return (int)status;   // the RP2350 refused it; a retry would only repeat that
        }
        // corrupt reply: it stays pending on the slave until clocked out whole, so ask again
        if (tries + 1 >= RESPONSE_TRIES) return DDRBANG_E_REPLY;
        d->retries++;
    }
}

// Every command is idempotent (STATUS, INIT, and READ/WRITE of the same
// bytes), so one the link mangled can simply be sent again.
static int transact(ddrbang_t *d, uint8_t op, uint32_t addr, uint32_t len,
                    const uint8_t *payload, uint32_t payload_len)
{
    for (int tries = 1;; tries++) {
        int e = exchange(d, op, addr, len, payload, payload_len);
        bool link_fault = e == DDRBANG_E_REPLY || e == DDRLINK_E_HDR_CRC ||
                          e == DDRLINK_E_DATA_CRC || e == DDRLINK_E_SHORT;
        if (!link_fault || tries >= COMMAND_TRIES) return e;
        d->retries++;
    }
}

int ddrbang_status(ddrbang_t *d, ddrlink_status_t *out)
{
    int e = transact(d, DDRLINK_STATUS, 0, 0, NULL, 0);
    if (e == DDRBANG_OK) ddrlink_get_status(d->t.rx_buf + DDRLINK_HDR, out);
    return e;
}

int ddrbang_dram_init(ddrbang_t *d)
{
    return transact(d, DDRLINK_INIT, 0, 0, NULL, 0);
}

static bool bad_args(uint32_t addr, size_t len)
{
    return addr % DDRLINK_ALIGN || len % DDRLINK_ALIGN || len > UINT32_MAX - addr;
}

int ddrbang_write(ddrbang_t *d, uint32_t addr, const void *src, size_t len)
{
    if (bad_args(addr, len)) return DDRBANG_E_ARG;
    const uint8_t *s = src;
    while (len) {
        uint32_t n = len > DDRLINK_MAX_LEN ? DDRLINK_MAX_LEN : (uint32_t)len;
        int e = transact(d, DDRLINK_WRITE, addr, n, s, n);
        if (e) return e;
        addr += n;
        s += n;
        len -= n;
    }
    return DDRBANG_OK;
}

int ddrbang_read(ddrbang_t *d, uint32_t addr, void *dst, size_t len)
{
    if (bad_args(addr, len)) return DDRBANG_E_ARG;
    uint8_t *o = dst;
    while (len) {
        uint32_t n = len > DDRLINK_MAX_LEN ? DDRLINK_MAX_LEN : (uint32_t)len;
        int e = transact(d, DDRLINK_READ, addr, n, NULL, 0);
        if (e) return e;
        memcpy(o, d->t.rx_buf + DDRLINK_HDR, n);
        addr += n;
        o += n;
        len -= n;
    }
    return DDRBANG_OK;
}

const char *ddrbang_strerror(int err)
{
    switch (err) {
    case DDRBANG_OK: return "ok";
    case DDRBANG_E_TIMEOUT: return "timed out waiting for READY";
    case DDRBANG_E_XFER: return "SPI transfer failed";
    case DDRBANG_E_REPLY: return "corrupt reply";
    case DDRBANG_E_ARG: return "misaligned or too long";
    default: return err > 0 ? ddrlink_err_str((uint8_t)err) : "unknown error";
    }
}
