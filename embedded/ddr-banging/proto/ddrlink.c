#include "ddrlink.h"

uint32_t ddrlink_crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    // zlib's CRC32 (reflected 0xEDB88320), bitwise: small, and fast enough
    // next to a bit-banged DRAM
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

uint32_t ddrlink_crc32(const uint8_t *p, size_t n)
{
    return ddrlink_crc32_update(0, p, n);
}

void ddrlink_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t ddrlink_get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void ddrlink_put_hdr(uint8_t out[DDRLINK_HDR], const ddrlink_hdr_t *h)
{
    out[0] = (uint8_t)h->magic;
    out[1] = (uint8_t)(h->magic >> 8);
    out[2] = h->op;
    out[3] = h->seq;
    ddrlink_put32(out + 4, h->addr);
    ddrlink_put32(out + 8, h->len);
    ddrlink_put32(out + 12, ddrlink_crc32(out, 12));
}

bool ddrlink_get_hdr(const uint8_t in[DDRLINK_HDR], ddrlink_hdr_t *h)
{
    h->magic = (uint16_t)(in[0] | in[1] << 8);
    h->op = in[2];
    h->seq = in[3];
    h->addr = ddrlink_get32(in + 4);
    h->len = ddrlink_get32(in + 8);
    return ddrlink_get32(in + 12) == ddrlink_crc32(in, 12);
}

void ddrlink_put_status(uint8_t out[DDRLINK_STATUS_LEN], const ddrlink_status_t *s)
{
    out[0] = (uint8_t)s->version;
    out[1] = (uint8_t)(s->version >> 8);
    out[2] = (uint8_t)s->flags;
    out[3] = (uint8_t)(s->flags >> 8);
    ddrlink_put32(out + 4, s->capacity);
    ddrlink_put32(out + 8, s->tck_ns);
    out[12] = (uint8_t)(uint16_t)s->temp16;
    out[13] = (uint8_t)((uint16_t)s->temp16 >> 8);
    out[14] = s->last_err;
    out[15] = s->reserved;
    ddrlink_put32(out + 16, s->frames);
    ddrlink_put32(out + 20, s->errors);
}

void ddrlink_get_status(const uint8_t in[DDRLINK_STATUS_LEN], ddrlink_status_t *s)
{
    s->version = (uint16_t)(in[0] | in[1] << 8);
    s->flags = (uint16_t)(in[2] | in[3] << 8);
    s->capacity = ddrlink_get32(in + 4);
    s->tck_ns = ddrlink_get32(in + 8);
    s->temp16 = (int16_t)(uint16_t)(in[12] | in[13] << 8);
    s->last_err = in[14];
    s->reserved = in[15];
    s->frames = ddrlink_get32(in + 16);
    s->errors = ddrlink_get32(in + 20);
}

size_t ddrlink_response_len(uint8_t op, uint32_t len)
{
    switch (op) {
    case DDRLINK_STATUS: return DDRLINK_HDR + DDRLINK_STATUS_LEN + DDRLINK_CRC;
    case DDRLINK_READ:   return DDRLINK_HDR + len + DDRLINK_CRC;
    default:             return DDRLINK_HDR;
    }
}

const char *ddrlink_err_str(uint8_t err)
{
    static const char *const names[] = {
        "ok", "header CRC", "data CRC", "unknown op", "misaligned", "out of range",
        "short frame", "not initialised", "DRAM failure",
    };
    return err < sizeof names / sizeof names[0] ? names[err] : "unknown error";
}
