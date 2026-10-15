// ESP-IDF SPI transport for ddrbang: SPI master, mode 3, plus the READY input.

#ifndef DDRBANG_SPI_H
#define DDRBANG_SPI_H

#include "driver/spi_master.h"
#include "esp_err.h"

#include "ddrbang.h"

typedef struct {
    spi_host_device_t host;   // e.g. SPI2_HOST
    int sck, mosi, miso, cs, ready;
    int clock_hz;             // <= ~12 MHz: the RP2350's SPI slave limit
} ddrbang_spi_config_t;

// Sets up the bus and fills *d, ready for ddrbang_status() etc. Allocates two
// DMA-capable frame buffers of DDRLINK_FRAME_MAX bytes.
esp_err_t ddrbang_spi_open(const ddrbang_spi_config_t *cfg, ddrbang_t *d);

#endif
