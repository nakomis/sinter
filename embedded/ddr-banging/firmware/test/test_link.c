// ESP32 link: proto/ddrlink.c, the slave (src/link.c) and the ESP32 client
// core (esp32-client/components/ddrbang) wired back to back through a fake
// SPI transport that can corrupt frames in either direction.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "ddrbang.h"
#include "ddrlink.h"
#include "link.h"

// ---- fake memory backend ----------------------------------------------------

#define MEM_BYTES (1u << 20)
static uint8_t mem[MEM_BYTES];
static bool mem_ready, mem_fail;

static bool be_init(void *c) { (void)c; mem_ready = true; return !mem_fail; }
static bool be_ready(void *c) { (void)c; return mem_ready; }
static uint32_t be_tck(void *c) { (void)c; return 1000; }
static int16_t be_temp(void *c) { (void)c; return 25 * 16 + 8; }

static bool be_write(void *c, uint32_t a, const uint8_t *d, uint32_t n)
{
    (void)c;
    if (mem_fail) return false;
    memcpy(mem + a, d, n);
    return true;
}

static bool be_read(void *c, uint32_t a, uint8_t *d, uint32_t n)
{
    (void)c;
    if (mem_fail) return false;
    memcpy(d, mem + a, n);
    return true;
}

static const link_backend_t backend = {
    be_init, be_write, be_read, be_ready, be_tck, be_temp, MEM_BYTES, NULL,
};

// ---- fake wire: master frames go straight into link_frame ------------------

static link_t slave;
static uint8_t slave_tx[DDRLINK_FRAME_MAX];
static uint8_t m_tx[DDRLINK_FRAME_MAX], m_rx[DDRLINK_FRAME_MAX];
static uint8_t wire[DDRLINK_FRAME_MAX];
static unsigned frame_no, corrupt_mosi_at, corrupt_miso_at, corrupt_every_miso;
static size_t corrupt_byte;

static bool w_xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t n)
{
    (void)c;
    frame_no++;
    size_t pend;
    const uint8_t *out = link_tx(&slave, &pend);
    if (rx) {
        memset(rx, 0, n);
        memcpy(rx, out, pend < n ? pend : n);
        if (frame_no == corrupt_miso_at || corrupt_every_miso) rx[corrupt_byte % n] ^= 0x10;
    }
    memcpy(wire, tx, n);
    if (frame_no == corrupt_mosi_at) wire[corrupt_byte % n] ^= 0x01;
    link_frame(&slave, wire, n);
    return true;
}

static bool w_ready(void *c, uint32_t ms) { (void)c; (void)ms; return true; }

static ddrbang_t client;

static void setup(void)
{
    link_init(&slave, &backend, slave_tx);
    ddrbang_transport_t t = { w_xfer, w_ready, NULL, m_tx, m_rx };
    ddrbang_init(&client, &t);
    memset(mem, 0, sizeof mem);
    mem_ready = mem_fail = false;
    frame_no = corrupt_mosi_at = corrupt_miso_at = corrupt_every_miso = 0;
    corrupt_byte = 0;
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 31 + seed + (i >> 9));
}

// ---- tests ----------------------------------------------------------------------

static void test_crc32_matches_zlib(void)
{
    CHECK_EQ(ddrlink_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    uint32_t c = ddrlink_crc32_update(0, (const uint8_t *)"1234", 4);
    CHECK_EQ(ddrlink_crc32_update(c, (const uint8_t *)"56789", 5), 0xCBF43926u);
}

static void test_header_round_trip_and_crc(void)
{
    uint8_t b[DDRLINK_HDR];
    ddrlink_hdr_t h = { DDRLINK_MAGIC_CMD, DDRLINK_READ, 7, 0x12345678, 4096 }, g;
    ddrlink_put_hdr(b, &h);
    CHECK(b[0] == 'D' && b[1] == 'B');
    CHECK(ddrlink_get_hdr(b, &g));
    CHECK_EQ(g.op, DDRLINK_READ);
    CHECK_EQ(g.seq, 7);
    CHECK_EQ(g.addr, 0x12345678);
    CHECK_EQ(g.len, 4096);
    b[5] ^= 4;
    CHECK(!ddrlink_get_hdr(b, &g));
}

static void test_status_before_init(void)
{
    setup();
    ddrlink_status_t s;
    CHECK_EQ(ddrbang_status(&client, &s), DDRBANG_OK);
    CHECK_EQ(s.version, DDRLINK_VERSION);
    CHECK_EQ(s.flags, 0);
    CHECK_EQ(s.capacity, MEM_BYTES);
    CHECK_EQ(s.tck_ns, 1000);
    CHECK_EQ(s.temp16, 25 * 16 + 8);
    CHECK_EQ(s.frames, 1);   // this STATUS
}

static void test_read_write_need_init(void)
{
    setup();
    uint8_t b[64];
    CHECK_EQ(ddrbang_read(&client, 0, b, 64), DDRLINK_E_NOT_INIT);
    CHECK_EQ(ddrbang_write(&client, 0, b, 64), DDRLINK_E_NOT_INIT);
    CHECK_EQ(ddrbang_dram_init(&client), DDRBANG_OK);
    ddrlink_status_t s;
    ddrbang_status(&client, &s);
    CHECK_EQ(s.flags, DDRLINK_F_INITIALISED);
    CHECK_EQ(s.errors, 2);
    CHECK_EQ(s.last_err, DDRLINK_E_NOT_INIT);
}

static void test_round_trip_across_frames(void)
{
    setup();
    ddrbang_dram_init(&client);
    static uint8_t in[200000], out[200000];   // > 3 frames of DDRLINK_MAX_LEN
    fill(in, sizeof in, 5);
    CHECK_EQ(ddrbang_write(&client, 4096, in, sizeof in), DDRBANG_OK);
    CHECK(memcmp(mem + 4096, in, sizeof in) == 0);
    CHECK_EQ(mem[4095], 0);
    CHECK_EQ(mem[4096 + sizeof in], 0);
    CHECK_EQ(ddrbang_read(&client, 4096, out, sizeof out), DDRBANG_OK);
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK_EQ(client.retries, 0);
}

static void test_alignment_and_range(void)
{
    setup();
    ddrbang_dram_init(&client);
    uint8_t b[16] = { 0 };
    CHECK_EQ(ddrbang_write(&client, 4, b, 8), DDRBANG_E_ARG);
    CHECK_EQ(ddrbang_read(&client, 0, b, 12), DDRBANG_E_ARG);
    CHECK_EQ(ddrbang_read(&client, MEM_BYTES - 8, b, 16), DDRLINK_E_RANGE);
    CHECK_EQ(ddrbang_read(&client, MEM_BYTES - 8, b, 8), DDRBANG_OK);
    CHECK_EQ(ddrbang_read(&client, 0xFFFFFFF0u, b, 8), DDRLINK_E_RANGE);
    CHECK_EQ(ddrbang_read(&client, 0xFFFFFFF8u, b, 16), DDRBANG_E_ARG);   // wraps
    // zero-length: nothing to do, nothing sent
    unsigned before = frame_no;
    CHECK_EQ(ddrbang_read(&client, 0, b, 0), DDRBANG_OK);
    CHECK_EQ(frame_no, before);
}

static void test_slave_rejects_bad_frames(void)
{
    setup();
    mem_ready = true;
    uint8_t f[DDRLINK_HDR + 32];
    ddrlink_hdr_t h = { DDRLINK_MAGIC_CMD, DDRLINK_WRITE, 1, 4, 8 }, r;
    size_t n;

    ddrlink_put_hdr(f, &h);                        // misaligned addr
    link_frame(&slave, f, DDRLINK_HDR + 12);
    ddrlink_get_hdr(link_tx(&slave, &n), &r);
    CHECK_EQ(r.addr, DDRLINK_E_ALIGN);
    CHECK_EQ(n, DDRLINK_HDR);

    h.addr = 0;
    h.len = 64;                                    // payload cut short
    ddrlink_put_hdr(f, &h);
    link_frame(&slave, f, DDRLINK_HDR + 32);
    ddrlink_get_hdr(link_tx(&slave, &n), &r);
    CHECK_EQ(r.addr, DDRLINK_E_SHORT);

    h.op = 99;                                     // unknown op
    ddrlink_put_hdr(f, &h);
    link_frame(&slave, f, DDRLINK_HDR);
    ddrlink_get_hdr(link_tx(&slave, &n), &r);
    CHECK_EQ(r.addr, DDRLINK_E_OP);
    CHECK_EQ(r.seq, 1);

    h.op = DDRLINK_STATUS;                         // header CRC broken
    ddrlink_put_hdr(f, &h);
    f[9] ^= 0x80;
    link_frame(&slave, f, DDRLINK_HDR);
    ddrlink_get_hdr(link_tx(&slave, &n), &r);
    CHECK_EQ(r.addr, DDRLINK_E_HDR_CRC);
    CHECK_EQ(slave.errors, 4);
}

static void test_response_frames(void)
{
    setup();
    uint8_t f[DDRLINK_HDR];
    ddrlink_hdr_t h = { DDRLINK_MAGIC_CMD, DDRLINK_STATUS, 3, 0, 0 };
    ddrlink_put_hdr(f, &h);
    link_frame(&slave, f, DDRLINK_HDR);
    size_t n;
    link_tx(&slave, &n);
    CHECK_EQ(n, ddrlink_response_len(DDRLINK_STATUS, 0));

    uint8_t zeros[64] = { 0 };
    link_frame(&slave, zeros, 20);                 // partial read
    link_frame(&slave, zeros, 64);                 // whole reply
    link_frame(&slave, zeros, 64);                 // and again: the reply is still there
    const uint8_t *p = link_tx(&slave, &n);
    CHECK_EQ(n, ddrlink_response_len(DDRLINK_STATUS, 0));
    ddrlink_hdr_t r;
    CHECK(ddrlink_get_hdr(p, &r));
    CHECK_EQ(r.seq, 3);
    CHECK_EQ(slave.frames, 1);                     // response frames aren't commands
    link_frame(&slave, zeros, 4);                  // runt frame: ignored
    CHECK_EQ(slave.frames, 1);
}

static void test_corrupt_write_data_is_resent(void)
{
    setup();
    ddrbang_dram_init(&client);
    uint8_t in[256];
    fill(in, sizeof in, 9);
    corrupt_mosi_at = frame_no + 1;                // the WRITE's command frame
    corrupt_byte = DDRLINK_HDR + 100;              // in the payload
    CHECK_EQ(ddrbang_write(&client, 512, in, sizeof in), DDRBANG_OK);
    CHECK(memcmp(mem + 512, in, sizeof in) == 0);
    CHECK_EQ(client.retries, 1);
    CHECK_EQ(slave.last_err, DDRLINK_E_DATA_CRC);
}

static void test_corrupt_header_is_resent(void)
{
    setup();
    ddrbang_dram_init(&client);
    uint8_t in[64];
    fill(in, sizeof in, 2);
    corrupt_mosi_at = frame_no + 1;
    corrupt_byte = 6;                              // the address field
    CHECK_EQ(ddrbang_write(&client, 64, in, sizeof in), DDRBANG_OK);
    CHECK(memcmp(mem + 64, in, sizeof in) == 0);
    CHECK_EQ(slave.last_err, DDRLINK_E_HDR_CRC);
}

static void test_corrupt_reply_is_reread(void)
{
    setup();
    ddrbang_dram_init(&client);
    fill(mem + 1024, 512, 3);
    uint8_t out[512];
    unsigned writes_before = slave.frames;
    corrupt_miso_at = frame_no + 2;                // the READ's response frame
    corrupt_byte = DDRLINK_HDR + 300;
    CHECK_EQ(ddrbang_read(&client, 1024, out, sizeof out), DDRBANG_OK);
    CHECK(memcmp(out, mem + 1024, sizeof out) == 0);
    CHECK_EQ(client.retries, 1);
    CHECK_EQ(slave.frames, writes_before + 1);     // re-read the reply, didn't re-send READ
}

static void test_gives_up_on_a_dead_link(void)
{
    setup();
    corrupt_every_miso = 1;
    corrupt_byte = 2;
    ddrlink_status_t s;
    CHECK_EQ(ddrbang_status(&client, &s), DDRBANG_E_REPLY);
    CHECK(client.retries > 0);
}

static void test_dram_failure_reported(void)
{
    setup();
    ddrbang_dram_init(&client);
    mem_fail = true;
    uint8_t b[8] = { 0 };
    CHECK_EQ(ddrbang_write(&client, 0, b, 8), DDRLINK_E_DRAM);
    CHECK_EQ(ddrbang_read(&client, 0, b, 8), DDRLINK_E_DRAM);
    CHECK_EQ(ddrbang_dram_init(&client), DDRLINK_E_DRAM);
    CHECK(strcmp(ddrbang_strerror(DDRLINK_E_DRAM), "DRAM failure") == 0);
}

void test_link(void)
{
    RUN(test_crc32_matches_zlib);
    RUN(test_header_round_trip_and_crc);
    RUN(test_status_before_init);
    RUN(test_read_write_need_init);
    RUN(test_round_trip_across_frames);
    RUN(test_alignment_and_range);
    RUN(test_slave_rejects_bad_frames);
    RUN(test_response_frames);
    RUN(test_corrupt_write_data_is_resent);
    RUN(test_corrupt_header_is_resent);
    RUN(test_corrupt_reply_is_reread);
    RUN(test_gives_up_on_a_dead_link);
    RUN(test_dram_failure_reported);
}
