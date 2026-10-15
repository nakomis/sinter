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
    jedec.c
    stubs/sim.c
    ../src/ddr3.c
    ../src/spd.c
    ../src/bus_gpio.c
    ../src/link.c
    ../src/dram.c
    ../../proto/ddrlink.c
    ../../esp32-client/components/ddrbang/ddrbang.c
)
# stubs/ stands in for the pico-sdk hardware headers
target_include_directories(run_tests PRIVATE ../src . stubs ../../proto
    ../../esp32-client/components/ddrbang/include)
target_link_libraries(run_tests PRIVATE m)
