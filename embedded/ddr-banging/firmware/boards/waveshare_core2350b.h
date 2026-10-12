// Waveshare Core2350B: RP2350B (QFN-80, 48 GPIOs), A2 stepping, 8 MB PSRAM
// on GP47. Not in pico-sdk 2.2.0, so defined here.
//
// No default UART: GP0/GP1 are DQ0/DQ1. stdio is USB only.

// pico_cmake_set PICO_PLATFORM=rp2350

#ifndef _BOARDS_WAVESHARE_CORE2350B_H
#define _BOARDS_WAVESHARE_CORE2350B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define WAVESHARE_CORE2350B
#define PICO_RP2350A 0   // RP2350B: GPIO 30..47 present

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

// Conservative: the real part may be larger, and a smaller setting still works
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

#define WAVESHARE_CORE2350B_PSRAM_CS_PIN 47

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
