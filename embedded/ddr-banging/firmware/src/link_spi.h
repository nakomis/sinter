// ESP32 link transport: SPI1 as a mode-3 slave with DMA both ways, CS edge
// interrupts, and the READY line. Frames are handed to link.c.

#ifndef LINK_SPI_H
#define LINK_SPI_H

#include <stdbool.h>

#include "link.h"

void link_spi_init(link_t *l);

// Call often from the main loop: acts on a frame once CS has risen.
void link_spi_poll(void);

// Hold READY low while the shell uses the DRAM, so the ESP32 waits.
void link_spi_busy(bool busy);

#endif
