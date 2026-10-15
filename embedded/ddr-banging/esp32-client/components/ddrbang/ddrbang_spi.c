#include "ddrbang_spi.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// One SPI transaction on the S3 carries at most 32 KB (an 18-bit bit count),
// so a frame goes out in chunks with CS held low in between.
#define CHUNK 16384

typedef struct {
    spi_device_handle_t dev;
    int ready;
} spi_ctx_t;

static spi_ctx_t ctx;

static bool xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_ctx_t *s = c;
    if (spi_device_acquire_bus(s->dev, portMAX_DELAY) != ESP_OK) return false;
    bool ok = true;
    for (size_t off = 0; off < n && ok; off += CHUNK) {
        size_t len = n - off < CHUNK ? n - off : CHUNK;
        spi_transaction_t t = {
            .flags = off + len < n ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = len * 8,
            .rxlength = rx ? len * 8 : 0,
            .tx_buffer = tx + off,
            .rx_buffer = rx ? rx + off : NULL,
        };
        ok = spi_device_polling_transmit(s->dev, &t) == ESP_OK;
    }
    spi_device_release_bus(s->dev);
    return ok;
}

static bool wait_ready(void *c, uint32_t timeout_ms)
{
    spi_ctx_t *s = c;
    int64_t start = esp_timer_get_time();
    // STATUS answers within microseconds; 64 KB of bursts takes seconds
    while (!gpio_get_level(s->ready)) {
        int64_t waited = esp_timer_get_time() - start;
        if (waited > (int64_t)timeout_ms * 1000) return false;
        if (waited < 2000) esp_rom_delay_us(10);
        else vTaskDelay(1);
    }
    return true;
}

esp_err_t ddrbang_spi_open(const ddrbang_spi_config_t *cfg, ddrbang_t *d)
{
    gpio_config_t r = {
        .pin_bit_mask = 1ull << cfg->ready,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   // no RP2350 attached = never ready
    };
    esp_err_t e = gpio_config(&r);
    if (e != ESP_OK) return e;

    spi_bus_config_t bus = {
        .sclk_io_num = cfg->sck,
        .mosi_io_num = cfg->mosi,
        .miso_io_num = cfg->miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK,
    };
    if ((e = spi_bus_initialize(cfg->host, &bus, SPI_DMA_CH_AUTO)) != ESP_OK) return e;
    spi_device_interface_config_t dev = {
        .mode = 3,
        .clock_speed_hz = cfg->clock_hz,
        .spics_io_num = cfg->cs,
        .queue_size = 1,
        .cs_ena_pretrans = 2,    // a little CS setup before the first SCK edge
        .cs_ena_posttrans = 2,
    };
    if ((e = spi_bus_add_device(cfg->host, &dev, &ctx.dev)) != ESP_OK) return e;
    ctx.ready = cfg->ready;

    uint8_t *tx = heap_caps_malloc(DDRLINK_FRAME_MAX, MALLOC_CAP_DMA);
    uint8_t *rx = heap_caps_malloc(DDRLINK_FRAME_MAX, MALLOC_CAP_DMA);
    if (!tx || !rx) return ESP_ERR_NO_MEM;
    ddrbang_transport_t t = { xfer, wait_ready, &ctx, tx, rx };
    ddrbang_init(d, &t);
    d->timeout_ms = 10000;   // 64 KB of bursts: ~0.2 s at 1 MHz, ~1 s at tCK 3.9 us
    return ESP_OK;
}
