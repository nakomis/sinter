cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_core2350b CACHE STRING "Board")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(pico_sdk_import.cmake)
project(ddr_banging C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(ddr_banging
    src/main.c
    src/bus_gpio.c
    src/ddr3.c
    src/spd.c
    src/dram.c
    src/link.c
    src/link_spi.c
    ../proto/ddrlink.c
)
target_include_directories(ddr_banging PRIVATE src ../proto)
target_compile_options(ddr_banging PRIVATE -Wall -Wextra)
target_link_libraries(ddr_banging pico_stdlib hardware_i2c hardware_gpio hardware_sync
    hardware_spi hardware_dma)

pico_enable_stdio_usb(ddr_banging 1)
pico_enable_stdio_uart(ddr_banging 0)
pico_add_extra_outputs(ddr_banging)
