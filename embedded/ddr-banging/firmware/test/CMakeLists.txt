# Host unit tests for the hardware-independent firmware modules.
cmake_minimum_required(VERSION 3.13)
project(ddr_banging_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror)

add_executable(run_tests
    run_tests.c
    test_ddr3.c
    test_spd.c
    ../src/ddr3.c
    ../src/spd.c
)
target_include_directories(run_tests PRIVATE ../src .)
