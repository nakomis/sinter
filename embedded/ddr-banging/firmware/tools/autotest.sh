#!/bin/zsh
# Wait for the board, flash it the moment BOOTSEL appears, then run the console tests
cd /Users/nakomis/repos/nakomis/sinter/embedded/ddr-banging/firmware
for i in {1..4800}; do
  if [[ -d /Volumes/RP2350 ]] || picotool info >/dev/null 2>&1; then
    echo "$(date +%T) BOOTSEL seen, flashing"; picotool load -x -f build/ddr_banging.uf2 2>&1 | tail -1; break
  fi
  sleep 0.25
done
for i in {1..80}; do P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); [[ -n $P ]] && break; sleep 0.25; done
[[ -z $P ]] && { echo "no serial port after flashing"; exit 1; }
sleep 1; echo "$(date +%T) port $P"
uv run -q -p 3.12 --with pyserial python "${0:A:h}/console.py" $P tck spd selftest selftest "bus pio" tck selftest "bus gpio" temp
