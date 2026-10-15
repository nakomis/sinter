// Milestone 10 test: an ESP32-S3 writes a pattern across the whole of the
// RP2350B's DDR3 chip, then reads every byte back and checks it.
// "An ESP32 with 128 MB of RAM."

#include <inttypes.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ddrbang.h"
#include "ddrbang_spi.h"

static const char *TAG = "ddrbang";

// ESP32-S3 FSPI pins (IO MUX, no GPIO matrix delay); READY anywhere.
static const ddrbang_spi_config_t PINS = {
    .host = SPI2_HOST,
    .sck = 12, .mosi = 11, .miso = 13, .cs = 10, .ready = 9,
    .clock_hz = 8 * 1000 * 1000,
};

#define CHUNK  DDRLINK_MAX_LEN
#define REPORT (4u << 20)   // progress every 4 MB

// Recomputable from the address alone: xorshift32 of the word index
static uint32_t word_at(uint32_t addr)
{
    uint32_t x = (addr >> 2) * 2654435761u + 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void fill(uint32_t *w, uint32_t addr, size_t len)
{
    for (size_t i = 0; i < len / 4; i++) w[i] = word_at(addr + 4 * i);
}

static void progress(const char *what, uint32_t done, uint32_t total, int64_t t0)
{
    double s = (esp_timer_get_time() - t0) / 1e6;
    ESP_LOGI(TAG, "%s %" PRIu32 " / %" PRIu32 " MB, %.1f KB/s, %.0f s", what, done >> 20,
             total >> 20, done / 1024.0 / s, s);
}

void app_main(void)
{
    ddrbang_t d;
    ESP_ERROR_CHECK(ddrbang_spi_open(&PINS, &d));

    ddrlink_status_t st;
    int e = ddrbang_status(&d, &st);
    if (e) {
        ESP_LOGE(TAG, "STATUS: %s (is the RP2350 running and wired?)", ddrbang_strerror(e));
        return;
    }
    ESP_LOGI(TAG, "RP2350 link v%u: %" PRIu32 " MB, tCK %" PRIu32 " ns, %s", st.version,
             st.capacity >> 20, st.tck_ns, st.flags & DDRLINK_F_INITIALISED ? "initialised" : "cold");
    if (st.temp16 != DDRLINK_NO_TEMP) ESP_LOGI(TAG, "module %.2f C", st.temp16 / 16.0);

    if ((e = ddrbang_dram_init(&d))) {
        ESP_LOGE(TAG, "INIT: %s", ddrbang_strerror(e));
        return;
    }

    uint32_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
    uint32_t *got = heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
    if (!buf || !got) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    uint32_t total = st.capacity;
    int64_t t0 = esp_timer_get_time();
    for (uint32_t a = 0; a < total; a += CHUNK) {
        fill(buf, a, CHUNK);
        if ((e = ddrbang_write(&d, a, buf, CHUNK))) {
            ESP_LOGE(TAG, "WRITE at 0x%08" PRIx32 ": %s", a, ddrbang_strerror(e));
            return;
        }
        if ((a + CHUNK) % REPORT == 0) progress("written", a + CHUNK, total, t0);
    }

    uint32_t bad = 0;
    t0 = esp_timer_get_time();
    for (uint32_t a = 0; a < total; a += CHUNK) {
        if ((e = ddrbang_read(&d, a, got, CHUNK))) {
            ESP_LOGE(TAG, "READ at 0x%08" PRIx32 ": %s", a, ddrbang_strerror(e));
            return;
        }
        fill(buf, a, CHUNK);
        for (size_t i = 0; i < CHUNK / 4; i++)
            if (got[i] != buf[i] && bad++ < 16)
                ESP_LOGW(TAG, "0x%08" PRIx32 ": want %08" PRIx32 " got %08" PRIx32,
                         (uint32_t)(a + 4 * i), buf[i], got[i]);
        if ((a + CHUNK) % REPORT == 0) progress("verified", a + CHUNK, total, t0);
    }

    ddrbang_status(&d, &st);
    ESP_LOGI(TAG, "%s: %" PRIu32 " bad words in %" PRIu32 " MB; %" PRIu32 " link errors, %u retries",
             bad ? "FAILED" : "PASSED", bad, total >> 20, st.errors, d.retries);
}
