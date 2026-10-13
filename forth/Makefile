# Sinter Forth compiler (SINT-25): host build and tests.
#
#   make          library, forthc and the test binary
#   make test     build and run the full test suite
#   make golden   regenerate tests/golden/*.golden from *.fs (review the
#                 diff, and the disassembly, before committing)
#   make clean
#
# Plain C11 with no dependencies beyond libc; builds with gcc or clang on
# Linux and macOS. Execution tests run only on x86-64 hosts and the
# objdump checks only where GNU objdump is installed; both report SKIP
# elsewhere rather than failing.

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -pedantic -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Isrc -Ihost
BUILD   := build

LIB_SRC  := src/compiler.c src/emit.c src/words.c
HOST_SRC := host/exec.c
TEST_SRC := tests/main.c tests/test_golden.c tests/test_errors.c \
            tests/test_disasm.c tests/test_exec.c

LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/%.o)
HOST_OBJ := $(HOST_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)

LIB    := $(BUILD)/libsinterforth.a
FORTHC := $(BUILD)/forthc
TESTS  := $(BUILD)/forth_tests

.PHONY: all test golden clean

all: $(LIB) $(FORTHC) $(TESTS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(FORTHC): $(BUILD)/tools/forthc.o $(HOST_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTS): $(TEST_OBJ) $(HOST_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

test: $(TESTS) $(FORTHC)
	./$(TESTS) tests

golden: $(FORTHC)
	@for f in tests/golden/*.fs; do \
	    echo "forthc -x $$f"; \
	    ./$(FORTHC) -x -o "$${f%.fs}.golden" "$$f" || exit 1; \
	done

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d) $(HOST_OBJ:.o=.d) $(TEST_OBJ:.o=.d) \
         $(BUILD)/tools/forthc.d
