# The protocol module is shared with the RP2350 firmware, one directory up.
idf_component_register(
    SRCS "ddrbang.c" "ddrbang_spi.c" "../../../proto/ddrlink.c"
    INCLUDE_DIRS "include" "../../../proto"
    REQUIRES esp_driver_spi esp_driver_gpio esp_timer
)
