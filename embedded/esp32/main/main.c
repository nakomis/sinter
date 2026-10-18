// Sinter orchestrator, stub (SINT-104): compiles a fixed Forth program to
// x86-64 on the ESP32 and logs the image. The Ollama client and the
// FPGA/PCIe write path come later; nothing here touches the Phenom.

#include <stdint.h>

#include "esp_log.h"

#include "sinter_forth.h"

static const char *TAG = "sinter";

static const char PROGRAM[] =
    ": fib dup 2 < if exit then dup 1- recurse swap 2 - recurse + ;\n"
    "20 fib result!\n";

// Both are too big for the main task's stack.
static forth_ctx ctx;
static uint8_t image[4096];

void app_main(void)
{
    if (forth_compile(&ctx, PROGRAM, sizeof PROGRAM - 1, image,
                      sizeof image) != FORTH_OK) {
        ESP_LOGE(TAG, "compile failed: %s", ctx.error.message);
        return;
    }
    ESP_LOGI(TAG, "compiled %u bytes of x86-64 (main at 0x%x, library %u)",
             (unsigned)ctx.code_len, (unsigned)ctx.main_offset,
             (unsigned)forth_library_size());
    ESP_LOG_BUFFER_HEXDUMP(TAG, image + ctx.main_offset,
                           ctx.code_len - ctx.main_offset, ESP_LOG_INFO);
}
