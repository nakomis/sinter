#include "link_spi.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "pins.h"

// Mode 3 (CPOL 1, CPHA 1): the PL022 only lets CS stay low across bytes when
// CPHA = 1. As a slave it needs clk_peri >= 12 x SCK: ~12 MHz at 150 MHz.
#define LINK_SPI spi1

static link_t *link;
static uint8_t rx_buf[DDRLINK_FRAME_MAX];
static int rx_ch = -1, tx_ch = -1;
static volatile bool frame_done;
static bool shell_busy;

static void cs_edge(uint gpio, uint32_t events)
{
    if (gpio != PIN_SPI_CS) return;
    if (events & GPIO_IRQ_EDGE_FALL) gpio_put(PIN_READY, 0);   // busy from the first byte
    if (events & GPIO_IRQ_EDGE_RISE) frame_done = true;
}

// Fresh SPI block (clears both FIFOs), RX DMA for a whole frame, TX DMA for
// the current reply, then READY.
static void arm(void)
{
    if (rx_ch >= 0) {
        dma_channel_abort(rx_ch);
        dma_channel_abort(tx_ch);
    }
    spi_init(LINK_SPI, 10 * 1000 * 1000);   // resets the block; baud unused as a slave
    spi_set_format(LINK_SPI, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    spi_set_slave(LINK_SPI, true);

    io_rw_32 *dr = &spi_get_hw(LINK_SPI)->dr;
    dma_channel_config c = dma_channel_get_default_config(rx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, spi_get_dreq(LINK_SPI, false));
    dma_channel_configure(rx_ch, &c, rx_buf, dr, sizeof rx_buf, true);

    size_t n;
    const uint8_t *tx = link_tx(link, &n);
    if (n) {
        c = dma_channel_get_default_config(tx_ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, spi_get_dreq(LINK_SPI, true));
        dma_channel_configure(tx_ch, &c, dr, tx, n, true);
    }
    gpio_put(PIN_READY, !shell_busy);
}

void link_spi_init(link_t *l)
{
    link = l;
    gpio_init(PIN_READY);
    gpio_put(PIN_READY, 0);
    gpio_set_dir(PIN_READY, true);
    gpio_set_function(PIN_SPI_RX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_TX, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_SPI_CS);   // no ESP32 attached: idle, not a stream of frames
    rx_ch = dma_claim_unused_channel(true);
    tx_ch = dma_claim_unused_channel(true);
    arm();
    // the pad's input stays live under the SPI function, so CS can interrupt too
    gpio_set_irq_enabled_with_callback(PIN_SPI_CS, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, cs_edge);
}

void link_spi_poll(void)
{
    if (!frame_done) return;
    frame_done = false;
    // CS is up, so no more bytes are coming: let the RX DMA empty the FIFO
    spi_hw_t *hw = spi_get_hw(LINK_SPI);
    for (int spin = 0; (hw->sr & SPI_SSPSR_RNE_BITS) && spin < 1000; spin++) tight_loop_contents();
    size_t n = sizeof rx_buf - dma_channel_hw_addr(rx_ch)->transfer_count;
    dma_channel_abort(rx_ch);
    dma_channel_abort(tx_ch);
    link_frame(link, rx_buf, n);
    arm();
}

void link_spi_busy(bool busy)
{
    shell_busy = busy;
    // a frame in flight has READY low already, and arm() will restore it
    if (!frame_done && gpio_get(PIN_SPI_CS)) gpio_put(PIN_READY, !busy);
}
