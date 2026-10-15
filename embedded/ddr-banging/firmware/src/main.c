// DDR-banging bring-up shell over USB serial. Between commands the DRAM
// waits in self-refresh, so the clock can stop.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "bus_gpio.h"
#include "ddr3.h"
#include "dram.h"
#include "link_spi.h"
#include "pins.h"
#include "spd.h"

#define TCK_MAX_NS 3400    // see cmd_tck

// ---- helpers --------------------------------------------------------------

static bool need_init(void)
{
    if (!dram_initialised()) printf("run 'init' first\n");
    return !dram_initialised();
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

// Module temperature in 1/16 C, or false if there's no thermal sensor.
static bool read_temp16(int *t)
{
    uint8_t reg = TSE_REG_TEMP, b[2];
    if (i2c_write_blocking(i2c1, TSE_I2C_ADDR, &reg, 1, true) != 1 ||
        i2c_read_blocking(i2c1, TSE_I2C_ADDR, b, 2, false) != 2)
        return false;
    *t = spd_tse_sixteenths((uint16_t)(b[0] << 8 | b[1]));
    return true;
}

static void cmd_temp(void)
{
    int t;
    if (!read_temp16(&t)) {
        printf("no thermal sensor at 0x%02x\n", TSE_I2C_ADDR);
        return;
    }
    printf("module %d.%02d C%s\n", t / 16, (abs(t) % 16) * 100 / 16,
           t >= 85 * 16 ? "  (>= 85 C: tREFI halves to 3.9 us)" : "");
}

static void report_dram_error(void)
{
    printf("failed: %s\n", dram_error() ? dram_error() : "unknown");
}

static void cmd_init(void)
{
    if (!dram_init()) {
        report_dram_error();
        return;
    }
    printf("initialised (DLL off, CL6, CWL6, BL8); MR0 %04x MR1 %04x MR2 %04x MR3 %04x; "
           "now in self-refresh\n", ddr3_mr0(), ddr3_mr1(), ddr3_mr2(), ddr3_mr3());
}

static void print_window(void)
{
    unsigned n;
    const bus_sample_t *samples = dram_samples(&n);
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

static void cmd_write(char *a, char *hex)
{
    uint8_t d[8];
    if (need_init()) return;
    if (!a || !parse_hex_bytes(hex, d)) {
        printf("usage: w <addr> <16 hex digits>\n");
        return;
    }
    uint32_t addr = strtoul(a, NULL, 0) & ~7u;
    if (!dram_write_burst(addr, d)) report_dram_error();
    else printf("wrote 8 bytes at 0x%08lx\n", (unsigned long)addr);
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
    if (!dram_read_burst(addr, d)) {
        report_dram_error();
        return;
    }
    print_window();
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
        if (!dram_write_burst(addr + 8 * i, d)) { report_dram_error(); return; }
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = addr + 8 * i;
        pattern(x, d);
        if (!dram_read_burst(x, got)) { report_dram_error(); return; }
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
        // DLL-off: tCK >= 8 ns, and no maximum beyond refresh. But no REF can
        // go in while a write's row is open (ACT to the next REF: 18 CK worst
        // case), and JEDEC allows only 8 postponed: 18 x tCK <= 8 x tREFI.
        if (ns < 250 || ns > TCK_MAX_NS) {
            printf("tck must be 250..%d ns\n", TCK_MAX_NS);
            return;
        }
        // changing the clock is allowed while the DRAM is in self-refresh
        uint32_t got = bus_set_tck((uint32_t)ns);
        if (got > (uint32_t)ns + (uint32_t)ns / 10)
            printf("note: the bit-bang loop can't go that fast\n");
        if (got > (uint32_t)TCK_MAX_NS) {
            printf("measured period %lu ns is too slow; back to 1000 ns\n", (unsigned long)got);
            bus_set_tck(1000);
        }
    }
    printf("tCK %lu ns (%.2f MHz)\n", (unsigned long)bus_tck(), 1000.0 / bus_tck());
}

static const struct { int gpio; const char *name; } lines[] = {
    { 0, "DQ0" }, { 1, "DQ1" }, { 2, "DQ2" }, { 3, "DQ3" }, { 4, "DQ4" }, { 5, "DQ5" },
    { 6, "DQ6" }, { 7, "DQ7" }, { 8, "DQS" }, { 9, "DQS#" }, { 10, "DIR_DQ" }, { 11, "DIR_DQS" },
    { PIN_RESETN, "RESET#" }, { 16, "CK" }, { 17, "CK#" }, { 18, "CKE" }, { 19, "CS0#" }, { 20, "CS1#" },
    { 21, "RAS#" }, { 22, "CAS#" }, { 23, "WE#" }, { 24, "BA0" }, { 25, "BA1" }, { 26, "BA2" },
    { 27, "A0" }, { 28, "A1" }, { 29, "A2" }, { 30, "A3" }, { 31, "A4" }, { 32, "A5" },
    { 33, "A6" }, { 34, "A7" }, { 35, "A8" }, { 36, "A9" }, { 37, "A10" }, { 38, "A11" },
    { 39, "A12" }, { 40, "A13" },
};
#define N_LINES (sizeof lines / sizeof lines[0])

static void cmd_hold(char *arg)
{
    dram_forget();   // the DRAM sees garbage from here
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
    dram_forget();
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
           "temp                module temperature (on-DIMM thermal sensor)\n"
           "init                reset + DLL-off mode-register init, then self-refresh\n"
           "w <addr> <hex16>    write one 8-byte burst\n"
           "r <addr>            read one burst, showing the raw sample window\n"
           "t [addr] [n]        write then verify n bursts of a pattern\n"
           "tck [ns]            show or set the clock period (250..3400)\n"
           "hold <line>|off     one line high, all others low (continuity)\n"
           "walk [ms]           step 'hold' through every line\n"
           "link                ESP32 link statistics\n");
}

// ---- ESP32 link -----------------------------------------------------------

static bool be_init(void *c) { (void)c; return dram_init(); }
static bool be_ready(void *c) { (void)c; return dram_initialised(); }
static uint32_t be_tck(void *c) { (void)c; return bus_tck(); }
static bool be_write(void *c, uint32_t a, const uint8_t *d, uint32_t n) { (void)c; return dram_write(a, d, n); }
static bool be_read(void *c, uint32_t a, uint8_t *d, uint32_t n) { (void)c; return dram_read(a, d, n); }

static int16_t be_temp(void *c)
{
    (void)c;
    int t;
    return read_temp16(&t) ? (int16_t)t : DDRLINK_NO_TEMP;
}

static const link_backend_t backend = {
    be_init, be_write, be_read, be_ready, be_tck, be_temp, DDR3_CHIP_BYTES, NULL,
};
static link_t link;
static uint8_t link_reply[DDRLINK_FRAME_MAX];

static void cmd_link(void)
{
    printf("ESP32 link: %lu command frames, %lu errors, last error: %s\n",
           (unsigned long)link.frames, (unsigned long)link.errors, ddrlink_err_str(link.last_err));
}

static void dispatch(char *line)
{
    char *cmd = strtok(line, " \t");
    char *a1 = strtok(NULL, " \t");
    char *a2 = strtok(NULL, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "spd")) cmd_spd();
    else if (!strcmp(cmd, "temp")) cmd_temp();
    else if (!strcmp(cmd, "init")) cmd_init();
    else if (!strcmp(cmd, "w")) cmd_write(a1, a2);
    else if (!strcmp(cmd, "r")) cmd_read(a1);
    else if (!strcmp(cmd, "t")) cmd_test(a1, a2);
    else if (!strcmp(cmd, "tck")) cmd_tck(a1);
    else if (!strcmp(cmd, "hold")) cmd_hold(a1);
    else if (!strcmp(cmd, "walk")) cmd_walk(a1);
    else if (!strcmp(cmd, "link")) cmd_link();
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
    link_init(&link, &backend, link_reply);
    link_spi_init(&link);

    char line[96];
    size_t len = 0;
    printf("\nddr-banging (SINT-100). 'help' for commands.\n> ");
    for (;;) {
        link_spi_poll();
        int ch = getchar_timeout_us(1000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            line[len] = 0;
            link_spi_busy(true);   // the ESP32 waits while the shell has the DRAM
            dispatch(line);
            link_spi_busy(false);
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
