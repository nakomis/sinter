// DDR-banging bring-up shell over USB serial. Between commands the DRAM
// waits in self-refresh, so the clock can stop.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "bus_gpio.h"
#include "ddr3.h"
#include "pins.h"
#include "spd.h"

#define SEQ_CAP 8192
static ddr3_cycle_t seq_buf[SEQ_CAP];
static bus_sample_t samples[64];
static bool initialised;

// ---- helpers --------------------------------------------------------------

static void seq_start(ddr3_seq_t *s)
{
    ddr3_seq_init(s, seq_buf, SEQ_CAP, bus_tck());
    s->cke = initialised;
}

static bool run(ddr3_seq_t *s, unsigned *n_samples)
{
    if (s->overflow) {
        printf("sequence too long (%u cycles max)\n", SEQ_CAP);
        return false;
    }
    bus_run(s, samples, 64);
    if (n_samples) {
        unsigned n = 0;
        for (size_t i = 0; i < s->len; i++) n += (s->cycles[i].flags & CY_SAMPLE) != 0;
        *n_samples = n;
    }
    return true;
}

static bool need_init(void)
{
    if (!initialised) printf("run 'init' first\n");
    return !initialised;
}

static bool parse_hex_bytes(const char *hex, uint8_t out[8])
{
    if (!hex || strlen(hex) != 16) return false;
    for (int i = 0; i < 8; i++) {
        char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char *end;
        out[i] = (uint8_t)strtoul(b, &end, 16);
        if (*end) return false;
    }
    return true;
}

// ---- commands -------------------------------------------------------------

static void cmd_spd(void)
{
    uint8_t raw[SPD_LEN], zero = 0;
    if (i2c_write_blocking(i2c1, SPD_I2C_ADDR, &zero, 1, true) != 1 ||
        i2c_read_blocking(i2c1, SPD_I2C_ADDR, raw, SPD_LEN, false) != SPD_LEN) {
        printf("no answer from the SPD at 0x%02x\n", SPD_I2C_ADDR);
        return;
    }
    for (int i = 0; i < SPD_LEN; i++)
        printf("%02x%s", raw[i], (i % 16 == 15) ? "\n" : " ");
    spd_info_t d;
    spd_decode(raw, &d);
    printf("DDR3: %s  CRC: %s (stored %04x, calc %04x)\n", d.is_ddr3 ? "yes" : "NO",
           d.crc_ok ? "ok" : "BAD", d.crc_stored, d.crc_calc);
    printf("part '%s', type %u, %lu MB, %u rank(s), x%u chips of %lu Mb, %u banks\n",
           d.part, d.module_type, (unsigned long)d.module_mbyte, d.ranks, d.device_width,
           (unsigned long)d.chip_mbit, d.banks);
    printf("rows %u bits, cols %u bits, bus %u%s, %s, tCKmin %lu ps, tAAmin %lu ps, CL mask %04x\n",
           d.row_bits, d.col_bits, d.bus_width, d.ecc ? "+ECC" : "",
           d.v1_5 ? "1.5 V" : "not 1.5 V", (unsigned long)d.tck_min_ps,
           (unsigned long)d.taa_min_ps, d.cas_latencies);
}

static void cmd_init(void)
{
    // RESET# low >= 200 us with CKE low, then 500 us before CKE goes high
    initialised = false;
    bus_reset(true);
    sleep_us(200);
    bus_reset(false);
    sleep_us(500);
    ddr3_seq_t s;
    seq_start(&s);
    ddr3_seq_init_mrs(&s);
    ddr3_seq_sre(&s);
    if (!run(&s, NULL)) return;
    initialised = true;
    printf("initialised (DLL off, CL6, CWL6, BL8); MR0 %04x MR1 %04x MR2 %04x MR3 %04x; "
           "%u cycles; now in self-refresh\n",
           ddr3_mr0(), ddr3_mr1(), ddr3_mr2(), ddr3_mr3(), (unsigned)s.len);
}

static void print_window(unsigned n)
{
    printf("beat  DQ  DQS DQS#\n");
    for (unsigned i = 0; i < n; i++)
        for (int b = 0; b < 2; b++) {
            uint16_t v = samples[i].beat[b];
            unsigned beat = 2 * i + b;
            bool data = beat >= 2 * DDR3_READ_WINDOW_BEFORE && beat < 2 * DDR3_READ_WINDOW_BEFORE + 8;
            printf("%3u   %02x   %u    %u%s\n", beat, v & 0xFF, (v >> 8) & 1, (v >> 9) & 1,
                   data ? "   <- expected data" : "");
        }
}

static bool read_burst(uint32_t addr, uint8_t out[8], unsigned *n)
{
    ddr3_seq_t s;
    seq_start(&s);
    ddr3_seq_srx(&s);
    ddr3_seq_read(&s, addr);
    ddr3_seq_sre(&s);
    if (!run(&s, n)) return false;
    for (int k = 0; k < 8; k++) {
        unsigned beat = 2 * DDR3_READ_WINDOW_BEFORE + k;
        out[k] = samples[beat / 2].beat[beat % 2] & 0xFF;
    }
    return true;
}

static bool write_burst(uint32_t addr, const uint8_t d[8])
{
    ddr3_seq_t s;
    seq_start(&s);
    ddr3_seq_srx(&s);
    ddr3_seq_write(&s, addr, d);
    ddr3_seq_sre(&s);
    return run(&s, NULL);
}

static void cmd_write(char *a, char *hex)
{
    uint8_t d[8];
    if (need_init()) return;
    if (!a || !parse_hex_bytes(hex, d)) {
        printf("usage: w <addr> <16 hex digits>\n");
        return;
    }
    uint32_t addr = strtoul(a, NULL, 0) & ~7u;
    if (write_burst(addr, d)) printf("wrote 8 bytes at 0x%08lx\n", (unsigned long)addr);
}

static void cmd_read(char *a)
{
    if (need_init()) return;
    if (!a) {
        printf("usage: r <addr>\n");
        return;
    }
    uint32_t addr = strtoul(a, NULL, 0) & ~7u;
    uint8_t d[8];
    unsigned n;
    if (!read_burst(addr, d, &n)) return;
    print_window(n);
    printf("0x%08lx:", (unsigned long)addr);
    for (int k = 0; k < 8; k++) printf(" %02x", d[k]);
    printf("\n");
}

static void pattern(uint32_t addr, uint8_t d[8])
{
    for (int k = 0; k < 8; k++) d[k] = (uint8_t)((addr >> 3) * 0x9E + k * 0x3B + (addr >> 11));
}

static void cmd_test(char *a, char *count)
{
    if (need_init()) return;
    uint32_t addr = a ? strtoul(a, NULL, 0) & ~7u : 0;
    uint32_t n = count ? strtoul(count, NULL, 0) : 16;
    uint8_t d[8], got[8];
    for (uint32_t i = 0; i < n; i++) {
        pattern(addr + 8 * i, d);
        if (!write_burst(addr + 8 * i, d)) return;
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = addr + 8 * i;
        pattern(x, d);
        unsigned m;
        if (!read_burst(x, got, &m)) return;
        if (memcmp(d, got, 8)) {
            if (bad++ < 8) {
                printf("0x%08lx want", (unsigned long)x);
                for (int k = 0; k < 8; k++) printf(" %02x", d[k]);
                printf("  got");
                for (int k = 0; k < 8; k++) printf(" %02x", got[k]);
                printf("\n");
            }
        }
    }
    printf("%lu of %lu bursts read back correctly\n", (unsigned long)(n - bad), (unsigned long)n);
}

static void cmd_tck(char *arg)
{
    if (arg) {
        long ns = strtol(arg, NULL, 0);
        // DLL-off: tCK >= 8 ns. Micron caps it at 7.8 us (3.9 us above 85 C);
        // above ~3.9 us refreshes can't keep pace and rows exceed tRAS(max).
        if (ns < 250 || ns > 3900) {
            printf("tck must be 250..3900 ns\n");
            return;
        }
        // changing the clock is allowed while the DRAM is in self-refresh
        uint32_t got = bus_set_tck((uint32_t)ns);
        if (got > (uint32_t)ns + (uint32_t)ns / 10)
            printf("note: the bit-bang loop can't go that fast\n");
        if (got > 3900) {
            printf("measured period %lu ns is too slow; back to 1000 ns\n", (unsigned long)got);
            bus_set_tck(1000);
        }
    }
    printf("tCK %lu ns (%.2f MHz)\n", (unsigned long)bus_tck(), 1000.0 / bus_tck());
}

static const struct { int gpio; const char *name; } lines[] = {
    { 0, "DQ0" }, { 1, "DQ1" }, { 2, "DQ2" }, { 3, "DQ3" }, { 4, "DQ4" }, { 5, "DQ5" },
    { 6, "DQ6" }, { 7, "DQ7" }, { 8, "DQS" }, { 9, "DQS#" }, { 10, "DIR_DQ" }, { 11, "DIR_DQS" },
    { 15, "RESET#" }, { 16, "CK" }, { 17, "CK#" }, { 18, "CKE" }, { 19, "CS0#" }, { 20, "CS1#" },
    { 21, "RAS#" }, { 22, "CAS#" }, { 23, "WE#" }, { 24, "BA0" }, { 25, "BA1" }, { 26, "BA2" },
    { 27, "A0" }, { 28, "A1" }, { 29, "A2" }, { 30, "A3" }, { 31, "A4" }, { 32, "A5" },
    { 33, "A6" }, { 34, "A7" }, { 35, "A8" }, { 36, "A9" }, { 37, "A10" }, { 38, "A11" },
    { 39, "A12" }, { 40, "A13" },
};
#define N_LINES (sizeof lines / sizeof lines[0])

static void cmd_hold(char *arg)
{
    initialised = false;   // the DRAM sees garbage from here
    if (!arg || !strcmp(arg, "off")) {
        bus_hold(-1);
        printf("all lines idle\n");
        return;
    }
    for (unsigned i = 0; i < N_LINES; i++)
        if (!strcasecmp(arg, lines[i].name)) {
            bus_hold(lines[i].gpio);
            printf("%s (GPIO %d) high, everything else low\n", lines[i].name, lines[i].gpio);
            return;
        }
    printf("unknown line '%s'\n", arg);
}

static void cmd_walk(char *arg)
{
    initialised = false;
    uint32_t ms = arg ? strtoul(arg, NULL, 0) : 2000;
    for (unsigned i = 0; i < N_LINES; i++) {
        bus_hold(lines[i].gpio);
        printf("%-8s GPIO %2d high\n", lines[i].name, lines[i].gpio);
        sleep_ms(ms);
    }
    bus_hold(-1);
    printf("walk done\n");
}

static void help(void)
{
    printf("spd                 read and decode the SPD EEPROM\n"
           "init                reset + DLL-off mode-register init, then self-refresh\n"
           "w <addr> <hex16>    write one 8-byte burst\n"
           "r <addr>            read one burst, showing the raw sample window\n"
           "t [addr] [n]        write then verify n bursts of a pattern\n"
           "tck [ns]            show or set the clock period (250..3900)\n"
           "hold <line>|off     one line high, all others low (continuity)\n"
           "walk [ms]           step 'hold' through every line\n");
}

static void dispatch(char *line)
{
    char *cmd = strtok(line, " \t");
    char *a1 = strtok(NULL, " \t");
    char *a2 = strtok(NULL, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "spd")) cmd_spd();
    else if (!strcmp(cmd, "init")) cmd_init();
    else if (!strcmp(cmd, "w")) cmd_write(a1, a2);
    else if (!strcmp(cmd, "r")) cmd_read(a1);
    else if (!strcmp(cmd, "t")) cmd_test(a1, a2);
    else if (!strcmp(cmd, "tck")) cmd_tck(a1);
    else if (!strcmp(cmd, "hold")) cmd_hold(a1);
    else if (!strcmp(cmd, "walk")) cmd_walk(a1);
    else help();
}

int main(void)
{
    stdio_init_all();
    bus_init();
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);   // weak; the fixture should have proper pull-ups
    gpio_pull_up(PIN_SCL);

    char line[96];
    size_t len = 0;
    printf("\nddr-banging (SINT-100). 'help' for commands.\n> ");
    for (;;) {
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            line[len] = 0;
            dispatch(line);
            len = 0;
            printf("> ");
        } else if ((ch == 8 || ch == 127) && len) {
            len--;
            printf("\b \b");
        } else if (len < sizeof line - 1 && ch >= ' ') {
            line[len++] = (char)ch;
            putchar(ch);
        }
    }
}
