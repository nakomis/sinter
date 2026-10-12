# make test   - host unit tests (no hardware)
# make        - firmware .uf2 for the Core2350B
# make flash  - load it with picotool (board in BOOTSEL, or running this firmware)

PICO_SDK_PATH ?= $(HOME)/repos/raspberrypi/pico-sdk
PICO_TOOLCHAIN_PATH ?= $(HOME)/opt/arm-gnu-toolchain-15.3.rel2-darwin-arm64-arm-none-eabi
export PICO_SDK_PATH PICO_TOOLCHAIN_PATH

.PHONY: all test flash clean

all:
	cmake -S . -B build
	cmake --build build -j

test:
	cmake -S test -B build-test
	cmake --build build-test
	./build-test/run_tests

flash: all
	picotool load -x -f build/ddr_banging.uf2

clean:
	rm -rf build build-test
