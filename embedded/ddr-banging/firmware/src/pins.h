// GPIO map: the channel board's (DESIGN.md §2 "Pin map"), so this firmware
// runs unchanged on SINT-101. Spike-only differences are marked.

#ifndef PINS_H
#define PINS_H

#define PIN_DQ0      0    // DQ0..DQ7 = GPIO 0..7
#define PIN_DQS      8
#define PIN_DQSN     9
#define PIN_DIR_DQ   10   // translator DIR: high = A (RP2350) -> B (DRAM)
#define PIN_DIR_DQS  11
#define PIN_LANE0    12   // lane decoder A/B/C (channel board only)
#define PIN_RESETN   12   // spike: RESET# on the decoder's first pin, which the
                          // spike lacks (channel board: I/O expander)
#define PIN_SPI_TX   15   // ESP32 link: SPI1 TX (MISO). The only free SPI1 TX
                          // pin: 47 is the Core2350B's PSRAM CSn
#define PIN_CA_BASE  16   // CK, CK#, CKE, CS0#, CS1#, RAS#, CAS#, WE#, BA0-2, A0-13 = 16..40
#define PIN_READY    41   // ESP32 link: high = ready for the next frame
#define PIN_SDA      42   // I2C1: SPD EEPROM at 0x50
#define PIN_SCL      43
#define PIN_SPI_RX   44   // ESP32 link: SPI1 RX (MOSI)
#define PIN_SPI_CS   45   // SPI1 CSn
#define PIN_SPI_SCK  46   // SPI1 SCK
#define PIN_PSRAM_CS 47   // Core2350B: on-board PSRAM CSn. Never drive it: the
                          // PSRAM would answer the flash's QSPI traffic

#define SPD_I2C_ADDR 0x50

#define DQ_MASK      (0xFFull << PIN_DQ0)
#define DQS_MASK     ((1ull << PIN_DQS) | (1ull << PIN_DQSN))
#define DIR_MASK     ((1ull << PIN_DIR_DQ) | (1ull << PIN_DIR_DQS))

#endif
