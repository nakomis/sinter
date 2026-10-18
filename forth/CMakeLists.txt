# ESP-IDF component "forth" (SINT-104), used via EXTRA_COMPONENT_DIRS by
# embedded/esp32. The host build is the Makefile; idf.py alone reads this.
idf_component_register(
    SRCS "src/compiler.c" "src/emit.c" "src/words.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
