#include "link_spi.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/spi.h"
#include "hardware/sync.h"

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
    // Registers by hand, not spi_init(): that enables the block as a mode-0
    // master first, driving SCK low against the ESP32's idle-high SCK.
    reset_unreset_block_num_wait_blocking(RESET_SPI1);
    spi_hw_t *hw = spi_get_hw(LINK_SPI);
    hw->cpsr = 2;                                   // unused as a slave, but must be even
    hw->cr0 = (8 - 1) << SPI_SSPCR0_DSS_LSB         // 8-bit, Motorola, mode 3
            | SPI_SSPCR0_SPO_BITS | SPI_SSPCR0_SPH_BITS;
    hw->cr1 = SPI_SSPCR1_MS_BITS;                   // slave, still disabled
    hw->dmacr = SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS;
    hw->cr1 = SPI_SSPCR1_MS_BITS | SPI_SSPCR1_SSE_BITS;

    io_rw_32 *dr = &spi_get_hw(LINK_SPI)->dr;
    dma_channel_config c = dma_channel_get_default_config(rx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, spi_get_dreq(LINK_SPI, false));
    dma_channel_configure(rx_ch, &c, rx_buf, dr, sizeof rx_buf, true);

    // TX runs for a whole frame whatever the reply's length (bytes past it are
    // ignored), so the TX FIFO never runs dry mid-frame: the PL022's slave
    // behaviour on underrun is undocumented.
    size_t n;
    const uint8_t *tx = link_tx(link, &n);
    c = dma_channel_get_default_config(tx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(LINK_SPI, true));
    dma_channel_configure(tx_ch, &c, dr, tx, DDRLINK_FRAME_MAX, true);
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
    // READY must drop within the shortest frame (16 bytes: ~16 us at 8 MHz)
    irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);
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
    // a frame in flight (or just ended) has READY low already, and arm() will
    // restore it; check both with the CS interrupt held off
    uint32_t irq = save_and_disable_interrupts();
    shell_busy = busy;
    if (!frame_done && gpio_get(PIN_SPI_CS)) gpio_put(PIN_READY, !busy);
    restore_interrupts(irq);
}
