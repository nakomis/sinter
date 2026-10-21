# Host unit tests for the hardware-independent firmware modules.
cmake_minimum_required(VERSION 3.13)
project(ddr_banging_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror)

add_executable(run_tests
    run_tests.c
    test_ddr3.c
    test_spd.c
    test_bus.c
    test_link.c
    test_jedec.c
    test_pio_enc.c
    test_pio_cmd.c
    test_pio_data.c
    test_pio_selftest.c
    pio_sim.c
    jedec.c
    dram_model.c
    stubs/sim.c
    ../src/ddr3.c
    ../src/spd.c
    ../src/bus_gpio.c
    ../src/link.c
    ../src/dram.c
    ../src/pio_enc.c
    ../src/pio_check.c
    ../../proto/ddrlink.c
    ../../esp32-client/components/ddrbang/ddrbang.c
)
# stubs/ stands in for the pico-sdk hardware headers
target_include_directories(run_tests PRIVATE ../src . stubs ../../proto
    ../../esp32-client/components/ddrbang/include)
target_link_libraries(run_tests PRIVATE m)

# ---- PIO: nakomis/epio (host PIO emulator) and pioasm ------------------------
# epio and apio are git submodules, pinned (git submodule update --init).
set(EPIO ${CMAKE_CURRENT_SOURCE_DIR}/third_party/epio)
set(APIO ${CMAKE_CURRENT_SOURCE_DIR}/third_party/apio)
file(GLOB EPIO_SRCS ${EPIO}/src/*.c)
add_library(epio STATIC ${EPIO_SRCS})
target_include_directories(epio PUBLIC ${EPIO}/include ${APIO}/include ${EPIO}/shim)
target_compile_definitions(epio PUBLIC APIO_EMULATION=1)
target_compile_options(epio PRIVATE -fshort-enums)   # epio's own flag; its API is safe without
target_link_libraries(run_tests PRIVATE epio)

# pioasm, built from the pico-sdk on the host, assembles the .pio files into
# the same headers the firmware build makes
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "pico-sdk, for pioasm")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/tools/pioasm/CMakeLists.txt")
    message(FATAL_ERROR "PICO_SDK_PATH ('${PICO_SDK_PATH}') isn't a pico-sdk: set it, or run 'make test'")
endif()
include(ExternalProject)
ExternalProject_Add(pioasm_host
    SOURCE_DIR ${PICO_SDK_PATH}/tools/pioasm
    BINARY_DIR ${CMAKE_BINARY_DIR}/pioasm
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DPIOASM_VERSION_STRING=2.2.0
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/pioasm/pioasm)
set(PIOASM ${CMAKE_BINARY_DIR}/pioasm/pioasm)
set(PIO_GEN ${CMAKE_BINARY_DIR}/pio)
file(MAKE_DIRECTORY ${PIO_GEN})
foreach(prog ddr3_cmd ddr3_wr ddr3_smp ddr3_cap)
    add_custom_command(OUTPUT ${PIO_GEN}/${prog}.pio.h
        COMMAND ${PIOASM} -o c-sdk ${CMAKE_CURRENT_SOURCE_DIR}/../src/pio/${prog}.pio ${PIO_GEN}/${prog}.pio.h
        DEPENDS pioasm_host ${CMAKE_CURRENT_SOURCE_DIR}/../src/pio/${prog}.pio)
    list(APPEND PIO_HEADERS ${PIO_GEN}/${prog}.pio.h)
endforeach()
add_custom_target(pio_headers DEPENDS ${PIO_HEADERS})
add_dependencies(run_tests pio_headers)
target_include_directories(run_tests PRIVATE ${PIO_GEN})
